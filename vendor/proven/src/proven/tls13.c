#include "proven_internal_tls.h"
#include "proven_internal_quic.h"
#include "../../platform/proven_sys_mem.h"

/* The TLS 1.3 state machine (RFC 8446), both roles. Bytes in, bytes out: no I/O, no clock and
 * no random source of its own - those come from the config.
 *
 * Everything that arrives is from a stranger. Every length is checked against what is left
 * before it is used, every message is checked against the state it arrives in, and the first
 * thing that is wrong ends the connection with the alert the RFC names for it. */

/* ---- Alerts (RFC 8446 section 6) ---- */
#define AL_CLOSE_NOTIFY 0
#define AL_UNEXPECTED_MESSAGE 10
#define AL_BAD_RECORD_MAC 20
#define AL_RECORD_OVERFLOW 22
#define AL_HANDSHAKE_FAILURE 40
#define AL_BAD_CERTIFICATE 42
#define AL_UNSUPPORTED_CERTIFICATE 43
#define AL_CERTIFICATE_EXPIRED 45
#define AL_ILLEGAL_PARAMETER 47
#define AL_UNKNOWN_CA 48
#define AL_DECODE_ERROR 50
#define AL_DECRYPT_ERROR 51
#define AL_PROTOCOL_VERSION 70
#define AL_INSUFFICIENT_SECURITY 71
#define AL_INTERNAL_ERROR 80
#define AL_USER_CANCELED 90
#define AL_MISSING_EXTENSION 109
#define AL_UNSUPPORTED_EXTENSION 110
#define AL_CERTIFICATE_REQUIRED 116
#define AL_NO_APPLICATION_PROTOCOL 120

/* ---- Handshake message types ---- */
#define HS_CLIENT_HELLO 1
#define HS_SERVER_HELLO 2
#define HS_NEW_SESSION_TICKET 4
#define HS_ENCRYPTED_EXTENSIONS 8
#define HS_CERTIFICATE 11
#define HS_END_OF_EARLY_DATA 5
#define HS_CERTIFICATE_REQUEST 13
#define HS_CERTIFICATE_VERIFY 15
#define HS_FINISHED 20
#define HS_KEY_UPDATE 24

/* ---- Extensions ---- */
#define EXT_SERVER_NAME 0
#define EXT_SUPPORTED_GROUPS 10
#define EXT_SIGNATURE_ALGORITHMS 13
#define EXT_ALPN 16
#define EXT_RECORD_SIZE_LIMIT 28
#define EXT_PRE_SHARED_KEY 41
#define EXT_SUPPORTED_VERSIONS 43
#define EXT_COOKIE 44
#define EXT_PSK_MODES 45
#define EXT_EARLY_DATA 42
#define EXT_KEY_SHARE 51
#define EXT_QUIC_TRANSPORT_PARAMETERS 0x39

#define GROUP_P256 0x0017
#define GROUP_X25519 0x001d

#define SIG_ECDSA_P256_SHA256 0x0403
#define SIG_ECDSA_P384_SHA384 0x0503
#define SIG_ED25519 0x0807
#define SIG_RSA_PSS_RSAE_SHA256 0x0804
#define SIG_RSA_PSS_RSAE_SHA384 0x0805
#define SIG_RSA_PSS_RSAE_SHA512 0x0806
#define SIG_RSA_PSS_PSS_SHA256 0x0809
#define SIG_RSA_PSS_PSS_SHA384 0x080a
#define SIG_RSA_PSS_PSS_SHA512 0x080b

/* ---- TLS 1.2 (RFC 5246) ---- */
#define HS12_HELLO_REQUEST 0
#define HS12_SERVER_KEY_EXCHANGE 12
#define HS12_SERVER_HELLO_DONE 14
#define HS12_CLIENT_KEY_EXCHANGE 16
#define EXT_EC_POINT_FORMATS 11
#define EXT_ENCRYPT_THEN_MAC 22
#define EXT_EXTENDED_MASTER_SECRET 23
#define EXT_SESSION_TICKET 35
#define EXT_RENEGOTIATION_INFO 0xff01
#define AL_NO_RENEGOTIATION 100
#define SIG_RSA_PKCS1_SHA256 0x0401
#define SIG_RSA_PKCS1_SHA384 0x0501
#define SIG_RSA_PKCS1_SHA512 0x0601
/* What a server that could have spoken 1.3 writes at the end of its random when it agrees to
 * 1.2 (RFC 8446 section 4.1.3): a client that offered 1.3 and sees it has been interfered with. */
static const proven_byte_t DOWNGRADE_SENTINEL[8] = { 0x44, 0x4f, 0x57, 0x4e, 0x47, 0x52, 0x44, 0x01 };

#define TLS_MAX_UNSENT ((proven_size_t)64 * 1024)
#define TLS_COOKIE_MAX 256
#define TLS_MAX_IGNORED 16                     /* records that carry nothing, tolerated in a row */
#define TLS_EARLY_SKIP 65536u                  /* bytes of refused early data a TCP server passes over */

static const proven_byte_t HRR_RANDOM[32] = {
    0xcf, 0x21, 0xad, 0x74, 0xe5, 0x9a, 0x61, 0x11, 0xbe, 0x1d, 0x8c, 0x02, 0x1e, 0x65, 0xb8, 0x91,
    0xc2, 0xa2, 0x11, 0x16, 0x7a, 0xbb, 0x8c, 0x5e, 0x07, 0x9e, 0x09, 0xe2, 0xc8, 0xa8, 0x33, 0x9c };

typedef enum {
    ST_C_WAIT_SH = 1, ST_C_WAIT_EE, ST_C_WAIT_CERT_CR, ST_C_WAIT_CERT, ST_C_WAIT_CV, ST_C_WAIT_FINISHED,
    ST_S_WAIT_CH, ST_S_WAIT_EOED, ST_S_WAIT_CERT, ST_S_WAIT_CV, ST_S_WAIT_FINISHED,
    ST_ESTABLISHED,
    /* TLS 1.2. A state named _CCS waits for the ChangeCipherSpec record, which is the one
     * place where the read keys change; no handshake message moves past it. */
    ST12_C_WAIT_CERT, ST12_C_WAIT_SKE, ST12_C_WAIT_DONE, ST12_C_WAIT_CCS, ST12_C_WAIT_FINISHED,
    ST12_S_WAIT_CERT, ST12_S_WAIT_CKE, ST12_S_WAIT_CV, ST12_S_WAIT_CCS, ST12_S_WAIT_FINISHED
} proven_tls_state_t;

typedef struct { proven_byte_t *ptr; proven_size_t len, cap; } proven_tls_buf_t;

/* What only the handshake needs. Freed, wiped, when it completes. */
typedef struct {
    proven_tls_transcript_t transcript;
    bool transcript_ready;                        /* false until the hash is known (client, before ServerHello) */
    proven_tls_buf_t first_hello;                        /* client: the ClientHello, kept until the hash is known */
    proven_tls_buf_t reassembly;                         /* a handshake message that spans records */
    proven_tls_buf_t scratch;                            /* a message being built */
    proven_byte_t random[32];                     /* client: its random, for the second ClientHello */
    proven_byte_t session_id[32];
    proven_size_t session_id_len;
    proven_byte_t x25519_priv[32];                /* SECRET */
    proven_byte_t p256_priv[32];                  /* SECRET */
    bool have_p256;
    bool retried;                                 /* a HelloRetryRequest has happened */
    bool ccs_sent;
    proven_u16 group;                             /* the group agreed */
    proven_byte_t cookie[TLS_COOKIE_MAX];
    proven_size_t cookie_len;
    proven_byte_t early_secret[PROVEN_HMAC_MAX_SIZE];
    proven_byte_t handshake_secret[PROVEN_HMAC_MAX_SIZE];
    proven_byte_t master_secret[PROVEN_HMAC_MAX_SIZE];
    proven_byte_t client_hs[PROVEN_HMAC_MAX_SIZE], server_hs[PROVEN_HMAC_MAX_SIZE];
    proven_byte_t psk[PROVEN_TLS_TICKET_PSK_MAX];
    proven_size_t psk_len;
    bool psk_offered, psk_accepted;
    bool client_early;                            /* server: the ClientHello offered early data */
    proven_u32 early_skip;                        /* server, TCP: bytes of refused early data still to pass over */
    proven_u32 client_age;                        /* server: the first identity's obfuscated_ticket_age */
    /* 0-RTT over TCP (RFC-0027). */
    bool want_early;                              /* client: offer it (proven_tls_client_create_early_) */
    bool ticket_sent;                             /* server: its ticket went with its Finished (RFC 8446 section 4.6.1) */
    proven_byte_t c_early[PROVEN_HMAC_MAX_SIZE];  /* client_early_traffic_secret. SECRET */
    const proven_tls_suite_t *early_suite;        /* the ticket's suite, which the early keys belong to */
    proven_u32 early_left;                        /* client: early bytes it may still write; server: may still take */
    proven_u32 early_written;                     /* client: early bytes written */
    bool cert_requested;                          /* client: the server asked for a certificate */
    bool client_cert_seen;                        /* server: the client presented one */
    proven_u16 peer_sig_schemes;                  /* a mask: bit 0 ecdsa p256, bit 1 ed25519 */
    proven_tls_buf_t peer_leaf;                          /* the peer's certificate, until CertificateVerify */
    proven_byte_t sni[254];
    proven_size_t sni_len;
    bool client_sent_alpn, client_sent_record_limit, client_sent_sni;
    proven_byte_t test_hello[600];                /* a test's ClientHello, sent instead of ours */
    proven_size_t test_hello_len;
    /* TLS 1.2 only. */
    proven_tls_transcript_t transcript_alt;       /* the handshake under the other hash: a CertificateVerify may need either */
    proven_byte_t server_random[32];
    proven_byte_t master12[PROVEN_TLS12_MASTER_SIZE];     /* SECRET */
    proven_tls_keys_t next_read;                  /* installed when the peer's ChangeCipherSpec arrives */
    proven_tls_keys_t next_write;                 /* server: installed when it sends its own */
    proven_u16 ske_group;
    bool etm;                                     /* encrypt_then_mac was agreed, for a CBC suite */
    bool ems;                                     /* the extended master secret was agreed (always, unless the legacy bit lets it go) */
    bool tr_old;                                  /* TLS 1.0/1.1: the handshake is hashed with MD5 and SHA-1 as well */
    proven_md5_t tr_md5;
    proven_sha1_t tr_sha1;
    proven_u16 hello_version;                     /* client: the version its hello offered, which an RSA premaster repeats */
    proven_byte_t dh_priv[PROVEN_TLS_DH_EXP_MAX]; /* DHE: this side's exponent. SECRET */
    proven_tls_buf_t dh;                                 /* DHE, client: its public value, then the shared secret */
    proven_size_t dh_pub_len;
    bool ticket_expected;                         /* client: the server said it will send a ticket */
    bool ticket_offered12;                        /* client: a 1.2 ticket went out in the hello */
    bool issue_ticket12;                          /* server: a ticket follows the client's Finished */
    bool resuming12;                              /* an abbreviated handshake: the master secret came from a ticket */
    proven_tls_buf_t ticket12;                           /* client: the ticket received, until the handshake completes */
    proven_u32 ticket12_lifetime;
} proven_tls_handshake_t;

#ifndef PROVEN_NO_HTTP3
/* QUIC mode (RFC 9001 section 4): handshake messages go to and come from the QUIC layer one
 * encryption level at a time, with no records around them; the secrets of each level are handed
 * over instead of being installed here; a failure is a QUIC error code instead of an alert on
 * the wire. Its own allocation, so that a connection over TCP pays one pointer for it. */
#define TLS_QUIC_TP_MAX 1024
typedef struct {
    proven_byte_t tp_local[TLS_QUIC_TP_MAX];      /* the transport parameters this side sends */
    proven_size_t tp_local_len;
    proven_quic_tp_t tp_peer;
    bool have_peer;
    proven_u8 read_level, write_level;            /* proven_quic_level_t */
    proven_tls_buf_t out[PROVEN_QUIC_LEVELS];            /* handshake bytes for the QUIC layer to send, per level */
    proven_size_t out_off[PROVEN_QUIC_LEVELS];
    proven_byte_t secret[PROVEN_QUIC_LEVELS][2][PROVEN_HMAC_MAX_SIZE];   /* [level][0 read, 1 write]; SECRET */
    bool secret_ready[PROVEN_QUIC_LEVELS][2];
    proven_u64 error;                             /* 0x0100 + alert, or a transport error code; 0: none */
    /* 0-RTT (RFC-0021): the early traffic secret (the client's to write, the server's to read)
     * and the suite it belongs to; where early data stands (PROVEN_TLS_EARLY_*). */
    proven_byte_t early_secret[PROVEN_HMAC_MAX_SIZE];   /* SECRET */
    const proven_tls_suite_t *early_suite;
    bool early_ready;
    proven_u8 early;
    int alpn_only;                                /* the one protocol of the configuration's list this connection may agree (its index), or -1: any */
    /* The QUIC version (RFC-0036): the one in use and the original - the client's first. A
     * ticket is offered and taken in the version it came from, which must be the original one
     * (RFC 9369 section 5); new tickets are of the version in use. A server moves to
     * `upgrade_to` (0: never) when the client's version_information lists it. */
    proven_u32 version, orig_version, upgrade_to;
} proven_tls_quic_t;
#endif

struct proven_tls_conn {
    const proven_tls_config_t *config;
    proven_allocator_t alloc;
    /* The small fields first and together, so that none of them costs padding: an idle
     * connection is held by the ten thousand. */
    bool is_server;
    bool established, dead, close_sent, close_received;
    bool error_reported;
    bool v12;
    bool resumed;
    bool has_peer_key;
    bool ticket_stored;                           /* client: a ticket has been kept since the handshake */
    bool take_early;                              /* server, TCP: its owner takes early data (proven_tls_server_take_early_) */
    bool in_early;                                /* server: the application data waiting arrived as early data */
    proven_u8 early;                              /* TCP: PROVEN_TLS_EARLY_* */
    proven_u16 early_read;                        /* server: application bytes read that arrived early (at most a ticket's 16,384) */
    proven_u32 read_total;                        /* application bytes read, held at its largest */
    proven_u16 version;                           /* 0 until agreed */
    proven_tls_state_t state;
    proven_err_t error;                           /* the failure, to be returned once */
    int alert_sent, alert_received;
    int alpn_index;                               /* into the config's list; -1 none */
    proven_u32 ignored;                           /* records in a row that carried nothing: see TLS_MAX_IGNORED */
    proven_cert_fault_t peer_fault;
    const proven_tls_suite_t *suite;
    const proven_tls12_suite_t *suite12;          /* TLS 1.2: instead of `suite` */
    /* The record keys, in an allocation of their own (RFC-0025 step 3): a quiet connection's are
     * wiped and freed by proven_tls_compact_, and `seed` keeps what remakes them - the key the
     * schedule began from, the IV and the sequence number - until the next call that needs them
     * (keys_live). Never compacted: QUIC mode, the CBC suites, a handshake in progress. */
    proven_tls_live_keys_t *live;
    proven_tls_key_seed_t *seed;                  /* while compact: [0] read, [1] write; a small allocation of its own */
    proven_tls_cbc_t *cbc;                        /* a CBC suite only: [0] what the client writes with, [1] the server */
    proven_byte_t read_secret[48], write_secret[48];      /* application traffic secrets, for KeyUpdate */
    proven_byte_t resumption_master[48];
    proven_size_t peer_record_limit;              /* the most plaintext the peer accepts in a record */
    proven_tls_handshake_t *hs;
    proven_tls_buf_t in;                                 /* a record being collected, or decrypted and not yet read */
    proven_size_t plain_off, plain_len;           /* undelivered application data inside `in` */
    proven_tls_buf_t out;
    proven_size_t out_off;
    proven_tls_buf_t post;                               /* a post-handshake message that spans records */
    proven_tls_session_t *session;
    proven_byte_t peer_key_hash[32];
    proven_tls_buf_t peer_cert;                          /* kept only when the config asks */
    proven_byte_t *server_name;                   /* client: the name it asked for; its own small allocation */
    proven_size_t server_name_len;
#ifndef PROVEN_NO_HTTP3
    proven_tls_quic_t *quic;                             /* QUIC mode; NULL over TCP */
#endif
};

static void tls_wipe(void *p, proven_size_t n) { proven_mem_wipe((proven_mem_mut_t){ .ptr = p, .size = n }); }

static bool buf_reserve(proven_tls_conn_t *c, proven_tls_buf_t *b, proven_size_t need) {
    if (need <= b->cap) return true;
    proven_size_t cap = b->cap ? b->cap : 256;
    while (cap < need) { if (cap > (proven_size_t)-1 / 2) return false; cap *= 2; }
    proven_result_mem_mut_t m = b->ptr ? c->alloc.realloc_fn(c->alloc.ctx, b->ptr, b->cap, cap, 16) : c->alloc.alloc_fn(c->alloc.ctx, cap, 16);
    if (m.err != PROVEN_OK) return false;
    b->ptr = m.value.ptr;
    b->cap = cap;
    return true;
}

static void buf_free(proven_tls_conn_t *c, proven_tls_buf_t *b) {
    if (b->ptr) { tls_wipe(b->ptr, b->cap); c->alloc.free_fn(c->alloc.ctx, b->ptr); }
    b->ptr = NULL; b->len = 0; b->cap = 0;
}

static bool buf_append(proven_tls_conn_t *c, proven_tls_buf_t *b, const proven_byte_t *p, proven_size_t n) {
    if (n > (proven_size_t)-1 - b->len || !buf_reserve(c, b, b->len + n)) return false;
    proven_sys_mem_copy(b->ptr + b->len, p, n);        /* a block: a byte at a time here was three cycles a byte of every record received */
    b->len += n;
    return true;
}

/* ---- Reading wire data: every get checks what is left ---- */

typedef struct { const proven_byte_t *p; proven_size_t n; bool ok; } proven_rd_t;

static proven_u32 rd_uint(proven_rd_t *r, int bytes) {
    if (!r->ok || r->n < (proven_size_t)bytes) { r->ok = false; return 0; }
    proven_u32 v = 0;
    for (int i = 0; i < bytes; ++i) v = (v << 8) | r->p[i];
    r->p += bytes; r->n -= (proven_size_t)bytes;
    return v;
}

static proven_rd_t rd_take(proven_rd_t *r, proven_size_t n) {
    proven_rd_t sub = { r->p, 0, false };
    if (!r->ok || r->n < n) { r->ok = false; return sub; }
    sub.n = n; sub.ok = true;
    r->p += n; r->n -= n;
    return sub;
}

/* A vector with a length prefix of 1, 2 or 3 bytes. */
static proven_rd_t rd_vec(proven_rd_t *r, int prefix) {
    proven_u32 n = rd_uint(r, prefix);
    return rd_take(r, n);
}

static bool bytes_eq(const proven_byte_t *a, const proven_byte_t *b, proven_size_t n) {
    proven_byte_t d = 0;
    for (proven_size_t i = 0; i < n; ++i) d |= (proven_byte_t)(a[i] ^ b[i]);
    return d == 0;
}

/* ---- Building wire data into the scratch buffer ---- */

typedef struct { proven_tls_conn_t *c; proven_tls_buf_t *b; bool ok; } proven_wr_t;

static void wr_bytes(proven_wr_t *w, const proven_byte_t *p, proven_size_t n) { if (w->ok && !buf_append(w->c, w->b, p, n)) w->ok = false; }
static void wr_uint(proven_wr_t *w, proven_u32 v, int bytes) {
    proven_byte_t t[4];
    for (int i = 0; i < bytes; ++i) t[i] = (proven_byte_t)(v >> (8 * (bytes - 1 - i)));
    wr_bytes(w, t, (proven_size_t)bytes);
}
/* Open a length-prefixed block; wr_close fills the length in. */
static proven_size_t wr_open(proven_wr_t *w, int prefix) { proven_size_t at = w->b->len; wr_uint(w, 0, prefix); return at; }
static void wr_close(proven_wr_t *w, proven_size_t at, int prefix) {
    if (!w->ok) return;
    proven_size_t n = w->b->len - at - (proven_size_t)prefix;
    for (int i = 0; i < prefix; ++i) w->b->ptr[at + (proven_size_t)i] = (proven_byte_t)(n >> (8 * (prefix - 1 - i)));
}

/* ---- Output ---- */

static bool out_reserve(proven_tls_conn_t *c, proven_size_t more) {
    if (c->out_off > 0 && c->out_off == c->out.len) { c->out.len = 0; c->out_off = 0; }
    return buf_reserve(c, &c->out, c->out.len + more);
}

/* One or more records carrying `data` as `type`, under the write keys if there are any. */
#ifndef PROVEN_NO_HTTP3
/* QUIC mode: a handshake message goes out at the level being written, as it is; an alert
 * becomes the connection's error code (unless a transport error was set first); nothing else
 * is ever sent. */
static bool quic_emit(proven_tls_conn_t *c, proven_byte_t type, const proven_byte_t *data, proven_size_t len) {
    proven_tls_quic_t *q = c->quic;
    if (type == PROVEN_TLS_CT_ALERT) {
        if (q->error == 0 && len == 2) q->error = PROVEN_QUIC_CRYPTO_ERROR + data[1];
        return true;
    }
    if (type != PROVEN_TLS_CT_HANDSHAKE) return false;
    proven_tls_buf_t *b = &q->out[q->write_level];
    if (q->out_off[q->write_level] > 0 && q->out_off[q->write_level] == b->len) { b->len = 0; q->out_off[q->write_level] = 0; }
    return buf_append(c, b, data, len);
}
#endif

static bool emit(proven_tls_conn_t *c, proven_byte_t type, const proven_byte_t *data, proven_size_t len, proven_byte_t clear_minor) {
#ifndef PROVEN_NO_HTTP3
    if (c->quic) return quic_emit(c, type, data, len);
#endif
    proven_size_t limit = c->live->write.active ? c->peer_record_limit : PROVEN_TLS_MAX_PLAINTEXT;
    /* TLS 1.0 chains each record's IV from the one before, so whoever can make this side send
     * chosen data knows the IV it will be encrypted under (BEAST). Sending one byte first puts
     * a MAC the attacker cannot predict in front of the rest. */
    bool split = type == PROVEN_TLS_CT_APPLICATION && len > 1 && c->live->write.active && c->cbc && c->cbc[c->is_server ? 1 : 0].chained;
    do {
        proven_size_t n = len < limit ? len : limit;
        if (split) { n = 1; split = false; }
        if (!out_reserve(c, n + PROVEN_TLS_CBC_MAX_EXPANSION)) return false;        /* 1.3: type and tag; 1.2: nonce and tag; CBC: the most */
        proven_byte_t *o = c->out.ptr + c->out.len;
        if (c->live->write.active && c->v12 && c->suite12->mac) {
            proven_byte_t iv[16];
            proven_tls_config_random(c->config, iv, sizeof iv);
            c->out.len += proven_tls_cbc_seal(&c->live->write, &c->cbc[c->is_server ? 1 : 0], type, (proven_mem_view_t){ .ptr = data, .size = n }, iv, o);
        } else if (c->live->write.active && c->v12) {
            c->out.len += proven_tls12_seal(&c->live->write, type, (proven_mem_view_t){ .ptr = data, .size = n }, o);
        } else if (c->live->write.active) {
            c->out.len += proven_tls13_seal(&c->live->write, type, (proven_mem_view_t){ .ptr = data, .size = n }, o);
        } else {
            /* Once an older version is agreed its records say so. */
            o[0] = type; o[1] = 0x03; o[2] = c->version != 0 && c->version < PROVEN_TLS_VERSION_1_2 ? (proven_byte_t)c->version : clear_minor;
            o[3] = (proven_byte_t)(n >> 8); o[4] = (proven_byte_t)n;
            for (proven_size_t i = 0; i < n; ++i) o[5 + i] = data[i];
            c->out.len += 5 + n;
        }
        data += n; len -= n;
    } while (len > 0);
    return true;
}

/* End the connection: tell the peer why, remember why. Returns the error for convenience. */
static proven_err_t tls_fail(proven_tls_conn_t *c, int alert, proven_err_t err) {
    if (c->dead) return c->error;
    proven_byte_t body[2] = { 2, (proven_byte_t)alert };
    (void)emit(c, PROVEN_TLS_CT_ALERT, body, 2, 0x03);
    c->alert_sent = alert;
    c->dead = true;
    c->error = err;
    return err;
}

static proven_err_t tls_nomem(proven_tls_conn_t *c) { return tls_fail(c, AL_INTERNAL_ERROR, PROVEN_ERR_NOMEM); }
static proven_err_t tls_decode(proven_tls_conn_t *c) { return tls_fail(c, AL_DECODE_ERROR, PROVEN_ERR_PROTOCOL); }
static proven_err_t tls_illegal(proven_tls_conn_t *c) { return tls_fail(c, AL_ILLEGAL_PARAMETER, PROVEN_ERR_PROTOCOL); }
static proven_err_t tls_unexpected(proven_tls_conn_t *c) { return tls_fail(c, AL_UNEXPECTED_MESSAGE, PROVEN_ERR_PROTOCOL); }

static bool is_quic(const proven_tls_conn_t *c) {
#ifndef PROVEN_NO_HTTP3
    return c->quic != NULL;
#else
    (void)c;
    return false;
#endif
}

#ifndef PROVEN_NO_HTTP3
/* End a QUIC-mode connection with a transport error code rather than an alert's. */
static proven_err_t quic_fail(proven_tls_conn_t *c, proven_u64 code) {
    if (!c->dead && c->quic->error == 0) c->quic->error = code;
    return tls_fail(c, AL_ILLEGAL_PARAMETER, PROVEN_ERR_PROTOCOL);
}
#endif

/* A level's traffic secret takes effect for reading or for writing. Over TCP its record keys are
 * installed; in QUIC mode the secret is handed to the QUIC layer and the level moves on. */
static void keys_read(proven_tls_conn_t *c, const proven_tls_suite_t *suite, const proven_byte_t *secret) {
#ifndef PROVEN_NO_HTTP3
    if (c->quic) {
        proven_tls_quic_t *q = c->quic;
        q->read_level++;
        for (proven_size_t i = 0; i < suite->hash_len; ++i) q->secret[q->read_level][0][i] = secret[i];
        q->secret_ready[q->read_level][0] = true;
        return;
    }
#endif
    proven_tls13_set_keys(&c->live->read, suite, secret);
}

static void keys_write(proven_tls_conn_t *c, const proven_tls_suite_t *suite, const proven_byte_t *secret) {
#ifndef PROVEN_NO_HTTP3
    if (c->quic) {
        proven_tls_quic_t *q = c->quic;
        q->write_level++;
        for (proven_size_t i = 0; i < suite->hash_len; ++i) q->secret[q->write_level][1][i] = secret[i];
        q->secret_ready[q->write_level][1] = true;
        return;
    }
#endif
    proven_tls13_set_keys(&c->live->write, suite, secret);
}

/* Send the handshake message in the scratch buffer and add it to the transcript. */
static bool hs_send(proven_tls_conn_t *c) {
    proven_tls_handshake_t *h = c->hs;
    if (h->transcript_ready) {
        proven_tls_transcript_update(&h->transcript, (proven_mem_view_t){ .ptr = h->scratch.ptr, .size = h->scratch.len });
        if (c->v12) proven_tls_transcript_update(&h->transcript_alt, (proven_mem_view_t){ .ptr = h->scratch.ptr, .size = h->scratch.len });
        if (h->tr_old) {
            proven_md5_update(&h->tr_md5, (proven_mem_view_t){ .ptr = h->scratch.ptr, .size = h->scratch.len });
            proven_sha1_update(&h->tr_sha1, (proven_mem_view_t){ .ptr = h->scratch.ptr, .size = h->scratch.len });
        }
    }
    return emit(c, PROVEN_TLS_CT_HANDSHAKE, h->scratch.ptr, h->scratch.len, 0x03);
}

/* Begin a handshake message in the scratch buffer; hs_end fills its length in. */
static proven_wr_t hs_begin(proven_tls_conn_t *c, proven_byte_t type) {
    proven_wr_t w = { c, &c->hs->scratch, true };
    c->hs->scratch.len = 0;
    wr_uint(&w, type, 1);
    wr_uint(&w, 0, 3);
    return w;
}

static bool hs_end(proven_wr_t *w) {
    if (!w->ok) return false;
    proven_size_t n = w->b->len - 4;
    w->b->ptr[1] = (proven_byte_t)(n >> 16); w->b->ptr[2] = (proven_byte_t)(n >> 8); w->b->ptr[3] = (proven_byte_t)n;
    return true;
}

static bool send_ccs(proven_tls_conn_t *c) {
    /* The compatibility ChangeCipherSpec travels in the clear even when keys are active. It
     * belongs to a handshake that carries a session id, and to no other. */
    if (c->hs->ccs_sent || c->hs->session_id_len == 0) return true;
    c->hs->ccs_sent = true;
    if (!out_reserve(c, 6)) return false;
    static const proven_byte_t ccs[6] = { PROVEN_TLS_CT_CCS, 0x03, 0x03, 0x00, 0x01, 0x01 };
    for (int i = 0; i < 6; ++i) c->out.ptr[c->out.len + (proven_size_t)i] = ccs[i];
    c->out.len += 6;
    return true;
}

/* ---- Suites, in the order this side prefers them ---- */

static proven_u16 g_test_suite_first;
static bool g_test_server_refuses_x25519;
void proven_tls_test_knobs(proven_u16 suite_first, bool server_refuses_x25519) {
    g_test_suite_first = suite_first;
    g_test_server_refuses_x25519 = server_refuses_x25519;
}
static bool g_test_quic_tp_tcp;
void proven_tls_test_quic_tp_tcp_(bool on) { g_test_quic_tp_tcp = on; }

/* Test knobs for TLS 1.2: bit 0, a server answers 1.2 though both sides could do 1.3 (what
 * interference with the hello would cause); bit 1, a client does not offer the extended
 * master secret; bit 2, a server does not answer it. */
static unsigned g_test12;
void proven_tls_test_knobs12(unsigned flags) { g_test12 = flags; }
static proven_u16 g_test_only12;
void proven_tls_test_only_suite12(proven_u16 id) { g_test_only12 = id; }
/* The count of records this side has written and the count the other has read, put where a
 * test says: a pair of connections that has carried 2^31 records, without carrying them. */
void proven_tls_test_records_(proven_tls_conn_t *writer, proven_tls_conn_t *reader, proven_u64 count);

/* The TLS 1.2 suite with the same cipher as a 1.3 suite, for an RSA key or for an EC one. */
/* A 1.2 suite this configuration may use: every legacy bit it needs is set. */
static bool suite12_allowed(const proven_tls_config_t *cfg, const proven_tls12_suite_t *suite) {
    return suite && (suite->legacy & ~cfg->legacy) == 0;
}

static proven_u16 suite12_for(proven_u16 id13, bool rsa) {
    if (id13 == PROVEN_TLS_AES_256_GCM_SHA384) return rsa ? 0xc030 : 0xc02c;
    if (id13 == PROVEN_TLS_CHACHA20_POLY1305_SHA256) return rsa ? 0xcca8 : 0xcca9;
    return rsa ? 0xc02f : 0xc02b;
}

static void suite_order(proven_u16 out[3]) {
    if (g_test_suite_first != 0) {
        static const proven_u16 all[3] = { PROVEN_TLS_AES_128_GCM_SHA256, PROVEN_TLS_AES_256_GCM_SHA384, PROVEN_TLS_CHACHA20_POLY1305_SHA256 };
        int n = 0;
        out[n++] = g_test_suite_first;
        for (int i = 0; i < 3; ++i) if (all[i] != g_test_suite_first && n < 3) out[n++] = all[i];
        return;
    }
    /* Without the processor's AES the bitsliced code is some thirty times slower than
     * ChaCha20: prefer ChaCha20 there. */
    if (proven_crypto_aes_hw_available()) {
        out[0] = PROVEN_TLS_AES_128_GCM_SHA256; out[1] = PROVEN_TLS_AES_256_GCM_SHA384; out[2] = PROVEN_TLS_CHACHA20_POLY1305_SHA256;
    } else {
        out[0] = PROVEN_TLS_CHACHA20_POLY1305_SHA256; out[1] = PROVEN_TLS_AES_128_GCM_SHA256; out[2] = PROVEN_TLS_AES_256_GCM_SHA384;
    }
}

/* ---- Key schedule steps ---- */

static void hs_hash(proven_tls_conn_t *c, proven_byte_t out[PROVEN_HMAC_MAX_SIZE]) { (void)proven_tls_transcript_hash(&c->hs->transcript, out); }

/* Early secret from the PSK (or from nothing), then the handshake secret and its traffic
 * secrets from the shared key. The transcript must end at the ServerHello. */
static void derive_handshake(proven_tls_conn_t *c, const proven_byte_t *shared, proven_size_t shared_len) {
    proven_tls_handshake_t *h = c->hs;
    proven_hmac_hash_t hash = c->suite->hash;
    proven_byte_t empty_hash[PROVEN_HMAC_MAX_SIZE], derived[PROVEN_HMAC_MAX_SIZE], th[PROVEN_HMAC_MAX_SIZE];
    proven_tls_transcript_t empty;
    proven_tls_transcript_init(&empty, hash);
    (void)proven_tls_transcript_hash(&empty, empty_hash);
    proven_mem_view_t psk = { .ptr = h->psk_accepted ? h->psk : NULL, .size = h->psk_accepted ? h->psk_len : 0 };
    proven_tls13_extract(hash, NULL, psk, h->early_secret);
    proven_tls13_derive_secret(hash, h->early_secret, "derived", empty_hash, derived);
    proven_tls13_extract(hash, derived, (proven_mem_view_t){ .ptr = shared, .size = shared_len }, h->handshake_secret);
    hs_hash(c, th);
    proven_tls13_derive_secret(hash, h->handshake_secret, "c hs traffic", th, h->client_hs);
    proven_tls13_derive_secret(hash, h->handshake_secret, "s hs traffic", th, h->server_hs);
    proven_tls13_derive_secret(hash, h->handshake_secret, "derived", empty_hash, derived);
    proven_tls13_extract(hash, derived, (proven_mem_view_t){ .ptr = NULL, .size = 0 }, h->master_secret);
    tls_wipe(derived, sizeof derived);
}

/* The application traffic secrets. The transcript must end at the server's Finished. */
static void derive_application(proven_tls_conn_t *c, proven_byte_t client_ap[PROVEN_HMAC_MAX_SIZE], proven_byte_t server_ap[PROVEN_HMAC_MAX_SIZE]) {
    proven_byte_t th[PROVEN_HMAC_MAX_SIZE];
    hs_hash(c, th);
    proven_tls13_derive_secret(c->suite->hash, c->hs->master_secret, "c ap traffic", th, client_ap);
    proven_tls13_derive_secret(c->suite->hash, c->hs->master_secret, "s ap traffic", th, server_ap);
}

/* The shared key for the agreed group. False for a peer key that must be refused. */
static bool key_agree(proven_tls_conn_t *c, proven_u16 group, proven_mem_view_t peer, proven_byte_t out[32]) {
    if (group == GROUP_X25519) return peer.size == 32 && proven_crypto_x25519(out, c->hs->x25519_priv, peer.ptr);
    if (group == GROUP_P256) return peer.size == 65 && proven_crypto_ecdh(PROVEN_CRYPTO_EC_P256, c->hs->p256_priv, peer, out);
    return false;
}

static void make_p256_key(proven_tls_conn_t *c, proven_byte_t pub[65]) {
    /* A scalar in range is all but certain on the first draw; the loop is for the rest. */
    for (;;) {
        proven_tls_config_random(c->config, c->hs->p256_priv, 32);
        if (proven_crypto_ec_public(PROVEN_CRYPTO_EC_P256, c->hs->p256_priv, pub)) break;
    }
    c->hs->have_p256 = true;
}

/* ---- The handshake's end ---- */

static void hs_free(proven_tls_conn_t *c) {
    proven_tls_handshake_t *h = c->hs;
    if (!h) return;
    buf_free(c, &h->first_hello); buf_free(c, &h->reassembly); buf_free(c, &h->scratch); buf_free(c, &h->peer_leaf); buf_free(c, &h->ticket12);
    if (h->dh.ptr) tls_wipe(h->dh.ptr, h->dh.cap);
    buf_free(c, &h->dh);
    tls_wipe(h, sizeof *h);
    c->alloc.free_fn(c->alloc.ctx, h);
    c->hs = NULL;
}

/* ---- Signatures over the transcript (RFC 8446 section 4.4.3) ---- */

static proven_size_t cv_content(proven_tls_conn_t *c, bool server_side, proven_byte_t out[64 + 34 + PROVEN_HMAC_MAX_SIZE]) {
    static const char server_ctx[] = "TLS 1.3, server CertificateVerify";
    static const char client_ctx[] = "TLS 1.3, client CertificateVerify";
    const char *ctx = server_side ? server_ctx : client_ctx;
    proven_size_t n = 0;
    for (int i = 0; i < 64; ++i) out[n++] = 0x20;
    for (int i = 0; i < 33; ++i) out[n++] = (proven_byte_t)ctx[i];
    out[n++] = 0;
    n += proven_tls_transcript_hash(&c->hs->transcript, out + n);
    return n;
}

static void digest_with(proven_hmac_hash_t hash, proven_mem_view_t data, proven_byte_t out[PROVEN_HMAC_MAX_SIZE]) {
    if (hash == PROVEN_HMAC_SHA256) proven_sha256(data, out);
    else if (hash == PROVEN_HMAC_SHA384) proven_sha384(data, out);
    else proven_sha512(data, out);
}

/* Is `sig` the signature of `msg` by the key in `leaf`, under `scheme`? 1 yes, 0 no, -1 when
 * the scheme is not one this side offered or not the one for that kind of key: a peer does not
 * get to choose a weaker pairing. PKCS #1 v1.5 is accepted only where `allow_pkcs1` says so -
 * in TLS 1.2, never in a TLS 1.3 CertificateVerify. */
static int sig_check(proven_u16 scheme, const proven_cert_t *leaf, proven_mem_view_t msg, proven_mem_view_t s, bool allow_pkcs1) {
    proven_byte_t digest[PROVEN_HMAC_MAX_SIZE];
    switch (scheme) {
        case SIG_ECDSA_P256_SHA256:
            if (leaf->key_kind != PROVEN_CERT_KEY_EC_P256) return -1;
            proven_sha256(msg, digest);
            return proven_crypto_ecdsa_verify_der(PROVEN_CRYPTO_EC_P256, leaf->key, (proven_mem_view_t){ .ptr = digest, .size = 32 }, s) ? 1 : 0;
        case SIG_ECDSA_P384_SHA384:
            if (leaf->key_kind != PROVEN_CERT_KEY_EC_P384) return -1;
            proven_sha384(msg, digest);
            return proven_crypto_ecdsa_verify_der(PROVEN_CRYPTO_EC_P384, leaf->key, (proven_mem_view_t){ .ptr = digest, .size = 48 }, s) ? 1 : 0;
        case SIG_ED25519:
            if (leaf->key_kind != PROVEN_CERT_KEY_ED25519) return -1;
            return s.size == 64 && proven_crypto_ed25519_verify(leaf->key.ptr, msg, s.ptr) ? 1 : 0;
        case SIG_RSA_PSS_RSAE_SHA256: case SIG_RSA_PSS_RSAE_SHA384: case SIG_RSA_PSS_RSAE_SHA512:
        case SIG_RSA_PSS_PSS_SHA256: case SIG_RSA_PSS_PSS_SHA384: case SIG_RSA_PSS_PSS_SHA512: {
            if (leaf->key_kind != PROVEN_CERT_KEY_RSA) return -1;
            int which = (scheme - (scheme >= SIG_RSA_PSS_PSS_SHA256 ? SIG_RSA_PSS_PSS_SHA256 : SIG_RSA_PSS_RSAE_SHA256));
            proven_hmac_hash_t hash = which == 0 ? PROVEN_HMAC_SHA256 : which == 1 ? PROVEN_HMAC_SHA384 : PROVEN_HMAC_SHA512;
            digest_with(hash, msg, digest);
            return proven_crypto_rsa_verify_pss(leaf->rsa_n, leaf->rsa_e, hash, proven_hmac_size(hash),
                                                (proven_mem_view_t){ .ptr = digest, .size = proven_hmac_size(hash) }, s) ? 1 : 0; }
        case SIG_RSA_PKCS1_SHA256: case SIG_RSA_PKCS1_SHA384: case SIG_RSA_PKCS1_SHA512: {
            if (!allow_pkcs1 || leaf->key_kind != PROVEN_CERT_KEY_RSA) return -1;
            proven_hmac_hash_t hash = scheme == SIG_RSA_PKCS1_SHA256 ? PROVEN_HMAC_SHA256 : scheme == SIG_RSA_PKCS1_SHA384 ? PROVEN_HMAC_SHA384 : PROVEN_HMAC_SHA512;
            digest_with(hash, msg, digest);
            return proven_crypto_rsa_verify_pkcs1(leaf->rsa_n, leaf->rsa_e, hash, (proven_mem_view_t){ .ptr = digest, .size = proven_hmac_size(hash) }, s) ? 1 : 0; }
        default:
            return -1;                            /* SHA-1 and everything else that was never offered */
    }
}

/* What TLS 1.0 and 1.1 sign: no scheme is named, the key's kind decides. An RSA key signs
 * MD5 | SHA-1 of the message, 36 bytes with no DigestInfo; an ECDSA key signs the SHA-1.
 * `digest` is those 36 bytes. 1 yes, 0 no, -1 a key that cannot sign there at all. */
static int sig_check_old(const proven_cert_t *leaf, const proven_byte_t digest[36], proven_mem_view_t s) {
    if (leaf->key_kind == PROVEN_CERT_KEY_RSA) return proven_crypto_rsa_verify_pkcs1_raw(leaf->rsa_n, leaf->rsa_e, (proven_mem_view_t){ .ptr = digest, .size = 36 }, s) ? 1 : 0;
    const proven_mem_view_t sha1 = { .ptr = digest + 16, .size = 20 };
    if (leaf->key_kind == PROVEN_CERT_KEY_EC_P256) return proven_crypto_ecdsa_verify_der(PROVEN_CRYPTO_EC_P256, leaf->key, sha1, s) ? 1 : 0;
    if (leaf->key_kind == PROVEN_CERT_KEY_EC_P384) return proven_crypto_ecdsa_verify_der(PROVEN_CRYPTO_EC_P384, leaf->key, sha1, s) ? 1 : 0;
    return -1;
}

static void sig_digest_old(proven_mem_view_t msg, proven_byte_t out[36]) {
    proven_md5(msg, out);
    proven_sha1(msg, out + 16);
}

/* Check the peer's CertificateVerify against the certificate it sent. */
static proven_err_t cv_verify(proven_tls_conn_t *c, proven_rd_t body, bool server_side) {
    proven_tls_handshake_t *h = c->hs;
    proven_u16 scheme = (proven_u16)rd_uint(&body, 2);
    proven_rd_t sig = rd_vec(&body, 2);
    if (!body.ok || body.n != 0 || sig.n == 0) return tls_decode(c);
    proven_cert_t leaf;
    if (h->peer_leaf.len == 0 || proven_cert_parse((proven_mem_view_t){ .ptr = h->peer_leaf.ptr, .size = h->peer_leaf.len }, &leaf) != PROVEN_OK) return tls_unexpected(c);
    proven_byte_t content[64 + 34 + PROVEN_HMAC_MAX_SIZE];
    proven_mem_view_t msg = { .ptr = content, .size = cv_content(c, server_side, content) };
    int ok = sig_check(scheme, &leaf, msg, (proven_mem_view_t){ .ptr = sig.p, .size = sig.n }, false);
    if (ok < 0) return tls_illegal(c);
    if (ok == 0) return tls_fail(c, AL_DECRYPT_ERROR, PROVEN_ERR_PROTOCOL);
    return PROVEN_OK;
}

static bool tls_sign_digest(proven_tls_conn_t *c, proven_u16 scheme, const proven_byte_t digest[32], proven_wr_t *w);

/* Sign `msg` with this side's key under `scheme`, and append the scheme, the length and the
 * signature. The scheme must be one for the key's kind; the caller chose it. */
static bool tls_sign(proven_tls_conn_t *c, proven_u16 scheme, proven_mem_view_t msg, proven_wr_t *w) {
    const proven_tls_config_t *cfg = c->config;
    if (cfg->key_kind == PROVEN_TLS_KEY_ED25519) {
        proven_byte_t sig[64];
        proven_crypto_ed25519_sign(sig, cfg->key, cfg->key_public, msg);
        wr_uint(w, scheme, 2); wr_uint(w, 64, 2); wr_bytes(w, sig, 64);
    } else {
        proven_byte_t digest[32];
        proven_sha256(msg, digest);
        return tls_sign_digest(c, scheme, digest, w);
    }
    return true;
}

/* The same for a message already hashed with SHA-256 - what a TLS 1.2 CertificateVerify signs
 * is the handshake so far, and only its hash is at hand. Not for an Ed25519 key, which signs
 * the message itself. */
static bool tls_sign_digest(proven_tls_conn_t *c, proven_u16 scheme, const proven_byte_t digest[32], proven_wr_t *w) {
    const proven_tls_config_t *cfg = c->config;
    if (cfg->key_kind == PROVEN_TLS_KEY_ED25519) return false;
    if (cfg->key_kind == PROVEN_TLS_KEY_RSA) {
        proven_byte_t sig[PROVEN_CRYPTO_RSA_SIGN_MAX_BYTES];
        proven_size_t n = 0;
        if (!proven_tls_config_rsa_sign_(cfg, scheme == SIG_RSA_PKCS1_SHA256, (proven_mem_view_t){ .ptr = digest, .size = 32 }, sig, &n)) return false;
        wr_uint(w, scheme, 2); wr_uint(w, (proven_u32)n, 2); wr_bytes(w, sig, n);
    } else {
        proven_byte_t raw[64], der[80];
        if (!proven_crypto_ecdsa_sign(PROVEN_CRYPTO_EC_P256, PROVEN_HMAC_SHA256, cfg->key, (proven_mem_view_t){ .ptr = digest, .size = 32 }, raw)) return false;
        /* SEQUENCE { INTEGER r, INTEGER s }, each minimal and non-negative. */
        proven_size_t n = 2;
        for (int part = 0; part < 2; ++part) {
            const proven_byte_t *v = raw + 32 * part;
            proven_size_t len = 32;
            while (len > 1 && v[0] == 0) { v++; len--; }
            der[n++] = 0x02; der[n++] = (proven_byte_t)(len + (v[0] >> 7));
            if (v[0] & 0x80) der[n++] = 0;
            for (proven_size_t i = 0; i < len; ++i) der[n++] = v[i];
        }
        der[0] = 0x30; der[1] = (proven_byte_t)(n - 2);
        wr_uint(w, scheme, 2); wr_uint(w, (proven_u32)n, 2); wr_bytes(w, der, n);
    }
    return true;
}

/* This side's signature in TLS 1.0 and 1.1 over the 36 bytes of sig_digest_old: its length
 * and the signature, with no scheme in front. Not for an Ed25519 key, which those versions
 * do not know. */
static bool tls_sign_old(proven_tls_conn_t *c, const proven_byte_t digest[36], proven_wr_t *w) {
    const proven_tls_config_t *cfg = c->config;
    if (cfg->key_kind == PROVEN_TLS_KEY_RSA) {
        proven_byte_t sig[PROVEN_CRYPTO_RSA_SIGN_MAX_BYTES];
        proven_size_t n = 0;
        if (!proven_tls_config_rsa_sign_(cfg, true, (proven_mem_view_t){ .ptr = digest, .size = 36 }, sig, &n)) return false;
        wr_uint(w, (proven_u32)n, 2); wr_bytes(w, sig, n);
        return true;
    }
    if (cfg->key_kind != PROVEN_TLS_KEY_P256) return false;
    proven_byte_t raw[64], der[80];
    if (!proven_crypto_ecdsa_sign(PROVEN_CRYPTO_EC_P256, PROVEN_HMAC_SHA256, cfg->key, (proven_mem_view_t){ .ptr = digest + 16, .size = 20 }, raw)) return false;
    proven_size_t n = 2;
    for (int part = 0; part < 2; ++part) {
        const proven_byte_t *v = raw + 32 * part;
        proven_size_t len = 32;
        while (len > 1 && v[0] == 0) { v++; len--; }
        der[n++] = 0x02; der[n++] = (proven_byte_t)(len + (v[0] >> 7));
        if (v[0] & 0x80) der[n++] = 0;
        for (proven_size_t i = 0; i < len; ++i) der[n++] = v[i];
    }
    der[0] = 0x30; der[1] = (proven_byte_t)(n - 2);
    wr_uint(w, (proven_u32)n, 2); wr_bytes(w, der, n);
    return true;
}

/* A ServerKeyExchange's signature, in the form of the agreed version. */
static bool ske_sign(proven_tls_conn_t *c, proven_u16 scheme, proven_mem_view_t msg, proven_wr_t *w) {
    if (c->version != 0 && c->version < PROVEN_TLS_VERSION_1_2) {
        proven_byte_t digest[36];
        sig_digest_old(msg, digest);
        return tls_sign_old(c, digest, w);
    }
    return tls_sign(c, scheme, msg, w);
}

/* The scheme this side's key signs a TLS 1.3 CertificateVerify with. For an RSA key it is
 * rsa_pss_rsae_sha256: the scheme every TLS 1.3 peer must accept (RFC 8446, 9.1). */
static proven_u16 our_scheme(const proven_tls_config_t *cfg) {
    return cfg->key_kind == PROVEN_TLS_KEY_ED25519 ? SIG_ED25519 : cfg->key_kind == PROVEN_TLS_KEY_RSA ? SIG_RSA_PSS_RSAE_SHA256 : SIG_ECDSA_P256_SHA256;
}

/* This side's CertificateVerify, into the scratch buffer. */
static bool cv_build(proven_tls_conn_t *c, bool server_side) {
    proven_byte_t content[64 + 34 + PROVEN_HMAC_MAX_SIZE];
    proven_mem_view_t msg = { .ptr = content, .size = cv_content(c, server_side, content) };
    proven_wr_t w = hs_begin(c, HS_CERTIFICATE_VERIFY);
    return tls_sign(c, our_scheme(c->config), msg, &w) && hs_end(&w);
}

/* The schemes this side accepts in a CertificateVerify and in certificates. */
static void wr_sig_algs(proven_wr_t *w) {
    static const proven_u16 list[] = {
        SIG_ECDSA_P256_SHA256, SIG_ECDSA_P384_SHA384, SIG_ED25519,
        SIG_RSA_PSS_RSAE_SHA256, SIG_RSA_PSS_RSAE_SHA384, SIG_RSA_PSS_RSAE_SHA512,
        SIG_RSA_PSS_PSS_SHA256, SIG_RSA_PSS_PSS_SHA384, SIG_RSA_PSS_PSS_SHA512,
        0x0401, 0x0501, 0x0601,                   /* RSA PKCS #1 v1.5: in certificates only */
    };
    wr_uint(w, EXT_SIGNATURE_ALGORITHMS, 2);
    proven_size_t ext = wr_open(w, 2), l = wr_open(w, 2);
    for (proven_size_t i = 0; i < sizeof list / sizeof list[0]; ++i) wr_uint(w, list[i], 2);
    wr_close(w, l, 2); wr_close(w, ext, 2);
}

/* Whether the peer's signature_algorithms list has the scheme this side's key signs with. */
static bool peer_accepts_our_key(const proven_tls_config_t *cfg, proven_rd_t list) {
    proven_u16 want = our_scheme(cfg);
    if (list.n % 2 != 0) return false;
    while (list.n >= 2) if ((proven_u16)rd_uint(&list, 2) == want) return true;
    return false;
}

/* This side's Certificate message into the scratch buffer; an empty list when `send` is false. */
static bool cert_build(proven_tls_conn_t *c, bool send) {
    const proven_tls_config_t *cfg = c->config;
    proven_wr_t w = hs_begin(c, HS_CERTIFICATE);
    if (!c->v12) wr_uint(&w, 0, 1);               /* certificate_request_context: empty. TLS 1.2 has none */
    proven_size_t list = wr_open(&w, 3);
    for (proven_size_t i = 0; send && i < cfg->chain_count; ++i) {
        wr_uint(&w, (proven_u32)cfg->chain[i].size, 3);
        wr_bytes(&w, cfg->chain[i].ptr, cfg->chain[i].size);
        if (!c->v12) wr_uint(&w, 0, 2);           /* no extensions */
    }
    wr_close(&w, list, 3);
    return hs_end(&w);
}

/* ---- The peer's certificate ---- */

static int alert_for_fault(proven_cert_fault_t f) {
    switch (f) {
        case PROVEN_CERT_FAULT_NO_ISSUER: return AL_UNKNOWN_CA;
        case PROVEN_CERT_FAULT_EXPIRED:
        case PROVEN_CERT_FAULT_NOT_YET_VALID: return AL_CERTIFICATE_EXPIRED;
        case PROVEN_CERT_FAULT_ALGORITHM: return AL_UNSUPPORTED_CERTIFICATE;
        default: return AL_BAD_CERTIFICATE;
    }
}

/* Parse and verify a Certificate message. *empty is set when the peer sent no certificate. */
static proven_err_t cert_process(proven_tls_conn_t *c, proven_rd_t body, bool *empty) {
    const proven_tls_config_t *cfg = c->config;
    proven_tls_handshake_t *h = c->hs;
    proven_mem_view_t chain[17];
    proven_size_t count = 0;
    *empty = false;
    /* TLS 1.3 puts a request context before the list and extensions after each certificate;
     * TLS 1.2 has neither. */
    proven_rd_t ctx = c->v12 ? (proven_rd_t){ body.p, 0, true } : rd_vec(&body, 1);
    proven_rd_t list = rd_vec(&body, 3);
    if (!body.ok || body.n != 0) return tls_decode(c);
    if (ctx.n != 0) return tls_illegal(c);
    while (list.n > 0) {
        proven_rd_t cert = rd_vec(&list, 3);
        proven_rd_t exts = c->v12 ? (proven_rd_t){ list.p, 0, true } : rd_vec(&list, 2);
        if (!list.ok || cert.n == 0) return tls_decode(c);
        (void)exts;                               /* per-certificate extensions (OCSP, SCT): not used */
        if (count < 17) chain[count++] = (proven_mem_view_t){ .ptr = cert.p, .size = cert.n };
    }
    if (count == 0) { *empty = true; return PROVEN_OK; }
    proven_cert_t leaf;
    if (proven_cert_parse(chain[0], &leaf) != PROVEN_OK) { c->peer_fault = PROVEN_CERT_FAULT_MALFORMED; return tls_fail(c, AL_BAD_CERTIFICATE, PROVEN_ERR_UNTRUSTED); }
    if (leaf.key_kind == PROVEN_CERT_KEY_UNKNOWN) { c->peer_fault = PROVEN_CERT_FAULT_ALGORITHM; return tls_fail(c, AL_UNSUPPORTED_CERTIFICATE, PROVEN_ERR_UNTRUSTED); }
    /* TLS 1.3 authenticates with a signature: a key restricted to something else cannot. */
    if (leaf.has_key_usage && !(leaf.key_usage & PROVEN_CERT_KU_DIGITAL_SIGNATURE)) { c->peer_fault = PROVEN_CERT_FAULT_USAGE; return tls_fail(c, AL_BAD_CERTIFICATE, PROVEN_ERR_UNTRUSTED); }
    (void)proven_cert_key_sha256(&leaf, c->peer_key_hash);
    c->has_peer_key = true;
    if (cfg->verify == PROVEN_TLS_VERIFY_CHAIN) {
        proven_cert_verify_options_t opt = {
            .anchors = cfg->anchors,
            .host = c->is_server ? (proven_u8str_view_t){ .ptr = NULL, .size = 0 } : (proven_u8str_view_t){ .ptr = c->server_name, .size = c->server_name_len },
            .now = proven_tls_config_now(cfg),
            .use = c->is_server ? PROVEN_CERT_USE_CLIENT : PROVEN_CERT_USE_SERVER,
        };
        proven_cert_verify_result_t res;
        proven_err_t e = cfg->anchors ? proven_cert_verify(chain, count, &opt, &res) : PROVEN_ERR_UNTRUSTED;
        if (e != PROVEN_OK) {
            c->peer_fault = cfg->anchors ? res.fault : PROVEN_CERT_FAULT_NO_ISSUER;
            return tls_fail(c, alert_for_fault(c->peer_fault), e);
        }
    }
    if (cfg->pin_count > 0) {
        bool pinned = false;
        for (proven_size_t i = 0; i < cfg->pin_count; ++i) pinned = pinned || bytes_eq(cfg->pins[i], c->peer_key_hash, 32);
        if (!pinned) { c->peer_fault = PROVEN_CERT_FAULT_PIN_MISMATCH; return tls_fail(c, AL_BAD_CERTIFICATE, PROVEN_ERR_UNTRUSTED); }
    }
    h->peer_leaf.len = 0;
    if (!buf_append(c, &h->peer_leaf, chain[0].ptr, chain[0].size)) return tls_nomem(c);
    if (cfg->keep_peer_certificate) {
        c->peer_cert.len = 0;
        if (!buf_append(c, &c->peer_cert, chain[0].ptr, chain[0].size)) return tls_nomem(c);
    }
    return PROVEN_OK;
}

static void tr_add(proven_tls_conn_t *c, proven_mem_view_t whole) {
    proven_tls_transcript_update(&c->hs->transcript, whole);
    if (c->v12) proven_tls_transcript_update(&c->hs->transcript_alt, whole);
    if (c->hs->tr_old) { proven_md5_update(&c->hs->tr_md5, whole); proven_sha1_update(&c->hs->tr_sha1, whole); }
}

/* The Finished message of this side, built and sent. */
static bool finished_send(proven_tls_conn_t *c, const proven_byte_t *base_secret) {
    proven_byte_t th[PROVEN_HMAC_MAX_SIZE], verify[PROVEN_HMAC_MAX_SIZE];
    hs_hash(c, th);
    proven_tls13_finished(c->suite->hash, base_secret, th, verify);
    proven_wr_t w = hs_begin(c, HS_FINISHED);
    wr_bytes(&w, verify, c->suite->hash_len);
    return hs_end(&w) && hs_send(c);
}

static proven_err_t finished_check(proven_tls_conn_t *c, proven_rd_t body, const proven_byte_t *base_secret) {
    proven_byte_t th[PROVEN_HMAC_MAX_SIZE], want[PROVEN_HMAC_MAX_SIZE];
    if (body.n != c->suite->hash_len) return tls_decode(c);
    hs_hash(c, th);
    proven_tls13_finished(c->suite->hash, base_secret, th, want);
    if (!proven_mem_equal_ct((proven_mem_view_t){ .ptr = want, .size = body.n }, (proven_mem_view_t){ .ptr = body.p, .size = body.n }))
        return tls_fail(c, AL_DECRYPT_ERROR, PROVEN_ERR_PROTOCOL);
    return PROVEN_OK;
}

/* The handshake is over: keep the application secrets, drop the rest. */
static void establish(proven_tls_conn_t *c, const proven_byte_t *read_ap, const proven_byte_t *write_ap) {
    proven_size_t n = c->suite->hash_len;
    for (proven_size_t i = 0; i < n; ++i) { c->read_secret[i] = read_ap[i]; c->write_secret[i] = write_ap[i]; }
    c->state = ST_ESTABLISHED;
    c->established = true;
}

/* ================= Client ================= */

static bool host_is_ip(const proven_byte_t *s, proven_size_t n) {
    /* An IP literal is not sent as a server name (RFC 6066). Colons mean IPv6; digits and dots
     * only mean IPv4. */
    bool digits = n > 0;
    for (proven_size_t i = 0; i < n; ++i) {
        if (s[i] == ':') return true;
        if (!((s[i] >= '0' && s[i] <= '9') || s[i] == '.')) digits = false;
    }
    return digits;
}

/* The session to offer, if there is one that may be offered to this server now. */
static proven_tls_session_data_t *client_session(proven_tls_conn_t *c) {
    if (!c->session || c->config->no_resumption) return NULL;
    _Static_assert(sizeof(proven_tls_session_data_t) <= PROVEN_TLS_SESSION_SIZE, "the session record must fit its public block");
    _Static_assert(alignof(proven_tls_session_data_t) <= alignof(proven_tls_session_t), "and be aligned in it");
    proven_tls_session_data_t *s = (proven_tls_session_data_t *)(void *)c->session->opaque;
    if (s->magic != PROVEN_TLS_SESSION_MAGIC) return NULL;
    const proven_tls_suite_t *suite = proven_tls_suite_find(s->suite);
    proven_i64 age = proven_tls_config_now(c->config) - s->received_at;
    if (!suite || s->psk_len != suite->hash_len || s->ticket_len == 0 || s->ticket_len > PROVEN_TLS_SESSION_TICKET_MAX) return NULL;
    if (age < 0 || age >= (proven_i64)s->lifetime_s || age >= 604800) return NULL;
    if (s->name_len != c->server_name_len || !bytes_eq(s->name, c->server_name, s->name_len)) return NULL;
#ifndef PROVEN_NO_HTTP3
    /* A ticket of the other QUIC version is not offered (RFC 9369 section 5). */
    if (is_quic(c) && (s->ticket_v2 != 0) != (c->quic->orig_version == PROVEN_QUIC_VERSION_2)) return NULL;
#endif
    return s;
}

/* The stored session, if it is a TLS 1.2 one that may be offered to this server now. What
 * tells the two kinds apart is the suite: no suite belongs to both versions. */
static proven_tls_session_data_t *client_session12(proven_tls_conn_t *c) {
    if (!c->session || c->config->no_resumption) return NULL;
    proven_tls_session_data_t *s = (proven_tls_session_data_t *)(void *)c->session->opaque;
    if (s->magic != PROVEN_TLS_SESSION_MAGIC) return NULL;
    proven_i64 age = proven_tls_config_now(c->config) - s->received_at;
    if (!suite12_allowed(c->config, proven_tls12_suite_find(s->suite)) || s->psk_len != PROVEN_TLS12_MASTER_SIZE || s->ticket_len == 0 || s->ticket_len > PROVEN_TLS_SESSION_TICKET_MAX) return NULL;
    if (age < 0 || age >= (proven_i64)s->lifetime_s || age >= 604800) return NULL;
    if (s->name_len != c->server_name_len || !bytes_eq(s->name, c->server_name, s->name_len)) return NULL;
    /* A session is resumed in the version it was made in: not offered where that cannot be agreed. */
    if (s->version < c->config->min_version || s->version > c->config->max_version) return NULL;
    return s;
}

/* Whether the configuration's protocol `i` may be agreed on this connection: in QUIC mode a
 * connection may be held to one (HTTP/3 to "h3", RFC 9114 section 3.1); otherwise any. */
static bool alpn_allowed(const proven_tls_conn_t *c, proven_size_t i) {
#ifndef PROVEN_NO_HTTP3
    if (c->quic && c->quic->alpn_only >= 0) return (proven_size_t)c->quic->alpn_only == i;
#else
    (void)c; (void)i;
#endif
    return true;
}

/* The protocol a session was made for is among those offered now: early data assumes it. Over
 * TCP a session made with no protocol agreed assumes none (RFC 8446 section 4.2.10). */
static bool session_alpn_offered(const proven_tls_conn_t *c, const proven_tls_config_t *cfg, const proven_tls_session_data_t *s) {
    if (s->alpn_len == 0 && !is_quic(c)) return true;
    for (proven_size_t i = 0; s->alpn_len > 0 && i < cfg->alpn_count; ++i) {
        if (!alpn_allowed(c, i)) continue;
        proven_u8str_view_t ours = proven_tls_config_alpn(cfg, i);
        if (ours.size == s->alpn_len && bytes_eq(ours.ptr, s->alpn, s->alpn_len)) return true;
    }
    return false;
}

static proven_err_t client_hello_send(proven_tls_conn_t *c) {
    proven_tls_handshake_t *h = c->hs;
    const proven_tls_config_t *cfg = c->config;
    if (h->test_hello_len > 0 && !h->retried) {
        h->scratch.len = 0;
        if (!buf_append(c, &h->scratch, h->test_hello, h->test_hello_len)) return tls_nomem(c);
        /* The session id is at a fixed place: after the type and length, the version and the random. */
        h->session_id_len = h->test_hello[38];
        for (proven_size_t i = 0; i < h->session_id_len && i < 32; ++i) h->session_id[i] = h->test_hello[39 + i];
        if (!buf_append(c, &h->first_hello, h->scratch.ptr, h->scratch.len)) return tls_nomem(c);
        return emit(c, PROVEN_TLS_CT_HANDSHAKE, h->scratch.ptr, h->scratch.len, 0x01) ? PROVEN_OK : tls_nomem(c);
    }
    proven_u16 suites[3];
    suite_order(suites);
    /* offer12: anything before 1.3 is on offer. six: TLS 1.2 itself is, and with it the AEAD suites. */
    /* QUIC carries TLS 1.3 and nothing older (RFC 9001 section 4.2). */
    const bool offer13 = cfg->max_version >= PROVEN_TLS_VERSION_1_3, offer12 = cfg->min_version <= PROVEN_TLS_VERSION_1_2 && !is_quic(c);
    const bool six = offer12 && cfg->max_version >= PROVEN_TLS_VERSION_1_2;
    h->hello_version = cfg->max_version < PROVEN_TLS_VERSION_1_2 ? cfg->max_version : PROVEN_TLS_VERSION_1_2;
    proven_tls_session_data_t *sess = offer13 ? client_session(c) : NULL;
    const proven_tls_suite_t *psk_suite = sess ? proven_tls_suite_find(sess->suite) : NULL;
    /* After a retry the server has fixed the suite: a session made under another hash cannot be offered. */
    if (sess && h->retried && psk_suite->hash != c->suite->hash) sess = NULL;
    /* Early data (RFC-0021): QUIC only, with a session that allows it for a protocol offered
     * now, and never after a retry (RFC 8446 section 4.1.2). */
    bool offer_early = false;
#ifndef PROVEN_NO_HTTP3
    offer_early = is_quic(c) && cfg->early_data && sess && sess->early && !h->retried && session_alpn_offered(c, cfg, sess);
    if (is_quic(c) && h->retried && c->quic->early == PROVEN_TLS_EARLY_OFFERED) c->quic->early = PROVEN_TLS_EARLY_REFUSED;
#endif
    /* Over TCP (RFC-0027): only on a connection made to offer it, with a ticket that allows it. */
    const bool tcp_early = !is_quic(c) && h->want_early && cfg->early_data && sess && sess->early && sess->early_max > 0 &&
                           !h->retried && session_alpn_offered(c, cfg, sess);

    proven_wr_t w = hs_begin(c, HS_CLIENT_HELLO);
    wr_uint(&w, h->hello_version, 2);
    wr_bytes(&w, h->random, 32);
    wr_uint(&w, (proven_u32)h->session_id_len, 1); wr_bytes(&w, h->session_id, h->session_id_len);
    {
        proven_size_t list = wr_open(&w, 2);
        for (int i = 0; offer13 && i < 3; ++i) wr_uint(&w, suites[i], 2);
        if (offer12) {
            /* The six ECDHE suites with an AEAD, in the order 1.3's three are in. */
            for (int i = 0; six && i < 3; ++i) {
                for (int rsa = 0; rsa < 2; ++rsa) {
                    const proven_u16 id = suite12_for(suites[i], rsa != 0);
                    if (g_test_only12 == 0 || g_test_only12 == id) wr_uint(&w, id, 2);
                }
            }
            /* Then whatever of the legacy set this configuration asked for, in the table's
             * order - only the suites with a SHA-1 MAC if nothing as new as 1.2 is on offer. */
            const proven_tls12_suite_t *s12;
            for (proven_size_t i = 0; (s12 = proven_tls12_suite_at(i)) != NULL; ++i) {
                if (g_test_only12 != 0 && g_test_only12 != s12->id) continue;
                const bool allowed = suite12_allowed(cfg, s12) || ((g_test12 & 16u) && g_test_only12 == s12->id);
                if (s12->legacy && allowed && (six || s12->mac == PROVEN_TLS_LH_SHA1)) wr_uint(&w, s12->id, 2);
            }
        }
        if (g_test12 & 8u) wr_uint(&w, 0x5600, 2);               /* TLS_FALLBACK_SCSV, which this client never sends by itself */
        wr_close(&w, list, 2);
    }
    wr_uint(&w, 1, 1); wr_uint(&w, 0, 1);         /* compression: none */
    proven_size_t exts = wr_open(&w, 2);

    if (c->server_name_len > 0 && !host_is_ip(c->server_name, c->server_name_len)) {
        wr_uint(&w, EXT_SERVER_NAME, 2);
        proven_size_t e = wr_open(&w, 2), l = wr_open(&w, 2);
        wr_uint(&w, 0, 1); wr_uint(&w, (proven_u32)c->server_name_len, 2); wr_bytes(&w, c->server_name, c->server_name_len);
        wr_close(&w, l, 2); wr_close(&w, e, 2);
    }
    {
        wr_uint(&w, EXT_SUPPORTED_VERSIONS, 2);
        proven_size_t e = wr_open(&w, 2), l = wr_open(&w, 1);
        /* Every version this configuration allows, newest first: a server that reads this
         * list reads nothing else. */
        for (proven_u16 v = PROVEN_TLS_VERSION_1_3; v >= PROVEN_TLS_VERSION_1_0; --v) if (v >= cfg->min_version && v <= cfg->max_version && (v == PROVEN_TLS_VERSION_1_3 || offer12)) wr_uint(&w, v, 2);
        wr_close(&w, l, 1); wr_close(&w, e, 2);
    }
    wr_uint(&w, EXT_SUPPORTED_GROUPS, 2); wr_uint(&w, 6, 2); wr_uint(&w, 4, 2); wr_uint(&w, GROUP_X25519, 2); wr_uint(&w, GROUP_P256, 2);
    wr_sig_algs(&w);
    if (offer12) {
        /* What a TLS 1.2 server needs to see. The extended master secret is not optional here:
         * a server that does not answer it is refused. renegotiation_info, empty, says this is
         * a first handshake and that this side knows the extension - it never renegotiates. */
        if (!(g_test12 & 2u)) { wr_uint(&w, EXT_EXTENDED_MASTER_SECRET, 2); wr_uint(&w, 0, 2); }
        wr_uint(&w, EXT_RENEGOTIATION_INFO, 2); wr_uint(&w, 1, 2); wr_uint(&w, 0, 1);
        wr_uint(&w, EXT_EC_POINT_FORMATS, 2); wr_uint(&w, 2, 2); wr_uint(&w, 1, 1); wr_uint(&w, 0, 1);
        /* With a CBC suite on offer: ask to MAC the ciphertext instead of the plaintext
         * (RFC 7366), which takes the padding out of reach of anyone who cannot forge a MAC. */
        if ((cfg->legacy & PROVEN_TLS_LEGACY_CBC) && !(g_test12 & 32u)) { wr_uint(&w, EXT_ENCRYPT_THEN_MAC, 2); wr_uint(&w, 0, 2); }
        if (!cfg->no_resumption) {
            /* A ticket from an earlier 1.2 session with this server, or an empty extension to
             * say that one would be accepted. (A session struct holds one session: if it is a
             * 1.3 one, it travels in pre_shared_key below instead.) */
            proven_tls_session_data_t *s12 = client_session12(c);
            wr_uint(&w, EXT_SESSION_TICKET, 2);
            wr_uint(&w, s12 ? s12->ticket_len : 0u, 2);
            if (s12) wr_bytes(&w, s12->ticket, s12->ticket_len);
            h->ticket_offered12 = s12 != NULL;
        }
    }
    if (offer13) {
        wr_uint(&w, EXT_KEY_SHARE, 2);
        proven_size_t e = wr_open(&w, 2), l = wr_open(&w, 2);
        if (h->retried && h->group == GROUP_P256) {
            proven_byte_t pub[65];
            make_p256_key(c, pub);
            wr_uint(&w, GROUP_P256, 2); wr_uint(&w, 65, 2); wr_bytes(&w, pub, 65);
        } else {
            proven_byte_t pub[32];
            proven_crypto_x25519_public(pub, h->x25519_priv);
            wr_uint(&w, GROUP_X25519, 2); wr_uint(&w, 32, 2); wr_bytes(&w, pub, 32);
        }
        wr_close(&w, l, 2); wr_close(&w, e, 2);
    }
    if (offer13 && !cfg->no_resumption) { wr_uint(&w, EXT_PSK_MODES, 2); wr_uint(&w, 2, 2); wr_uint(&w, 1, 1); wr_uint(&w, 1, 1); }   /* psk_dhe_ke */
    if (cfg->alpn_count > 0) {
        wr_uint(&w, EXT_ALPN, 2);
        proven_size_t e = wr_open(&w, 2), l = wr_open(&w, 2);
#ifndef PROVEN_NO_HTTP3
        if (c->quic && c->quic->alpn_only >= 0) {
            /* Held to one protocol: only that one is offered. */
            const proven_u8str_view_t only = proven_tls_config_alpn(cfg, (proven_size_t)c->quic->alpn_only);
            wr_uint(&w, (proven_u32)only.size, 1); wr_bytes(&w, only.ptr, only.size);
        } else
#endif
        wr_bytes(&w, cfg->alpn_wire, cfg->alpn_wire_len);
        wr_close(&w, l, 2); wr_close(&w, e, 2);
    }
    /* QUIC has no records to limit. It has transport parameters instead. */
    if (!is_quic(c)) { wr_uint(&w, EXT_RECORD_SIZE_LIMIT, 2); wr_uint(&w, 2, 2); wr_uint(&w, PROVEN_TLS_MAX_PLAINTEXT + 1, 2); }
#ifndef PROVEN_NO_HTTP3
    if (c->quic) {
        wr_uint(&w, EXT_QUIC_TRANSPORT_PARAMETERS, 2); wr_uint(&w, (proven_u32)c->quic->tp_local_len, 2);
        wr_bytes(&w, c->quic->tp_local, c->quic->tp_local_len);
    }
#endif
    if (h->cookie_len > 0) {
        wr_uint(&w, EXT_COOKIE, 2); wr_uint(&w, (proven_u32)h->cookie_len + 2, 2);
        wr_uint(&w, (proven_u32)h->cookie_len, 2); wr_bytes(&w, h->cookie, h->cookie_len);
    }
    if (offer_early || tcp_early) { wr_uint(&w, EXT_EARLY_DATA, 2); wr_uint(&w, 0, 2); }
    proven_size_t binder_at = 0;
    h->psk_offered = false;
    if (sess) {
        /* pre_shared_key is last: the binder covers everything before it. */
        proven_u32 age_ms = (proven_u32)((proven_u64)(proven_tls_config_now(cfg) - sess->received_at) * 1000u) + sess->age_add;
        wr_uint(&w, EXT_PRE_SHARED_KEY, 2);
        proven_size_t e = wr_open(&w, 2), ids = wr_open(&w, 2);
        wr_uint(&w, sess->ticket_len, 2); wr_bytes(&w, sess->ticket, sess->ticket_len); wr_uint(&w, age_ms, 4);
        wr_close(&w, ids, 2);
        binder_at = w.b->len;
        proven_byte_t zeros[PROVEN_HMAC_MAX_SIZE] = { 0 };
        wr_uint(&w, (proven_u32)psk_suite->hash_len + 1, 2); wr_uint(&w, (proven_u32)psk_suite->hash_len, 1); wr_bytes(&w, zeros, psk_suite->hash_len);
        wr_close(&w, e, 2);
        h->psk_offered = true;
        h->psk_len = sess->psk_len;
        for (proven_size_t i = 0; i < sess->psk_len; ++i) h->psk[i] = sess->psk[i];
    }
    wr_close(&w, exts, 2);
    if (!hs_end(&w)) return tls_nomem(c);
    if (h->psk_offered) {
        /* binder = HMAC(finished_key(binder_key), Hash(transcript so far + the hello up to here)) */
        proven_hmac_hash_t hash = psk_suite->hash;
        proven_tls_transcript_t t, empty;
        proven_byte_t early[PROVEN_HMAC_MAX_SIZE], binder_key[PROVEN_HMAC_MAX_SIZE], th[PROVEN_HMAC_MAX_SIZE], binder[PROVEN_HMAC_MAX_SIZE];
        if (h->transcript_ready) t = h->transcript; else proven_tls_transcript_init(&t, hash);
        proven_tls_transcript_update(&t, (proven_mem_view_t){ .ptr = h->scratch.ptr, .size = binder_at });
        (void)proven_tls_transcript_hash(&t, th);
        proven_tls_transcript_init(&empty, hash);
        (void)proven_tls_transcript_hash(&empty, binder);
        proven_tls13_extract(hash, NULL, (proven_mem_view_t){ .ptr = h->psk, .size = h->psk_len }, early);
        proven_tls13_derive_secret(hash, early, "res binder", binder, binder_key);
        proven_tls13_finished(hash, binder_key, th, binder);
        for (proven_size_t i = 0; i < psk_suite->hash_len; ++i) h->scratch.ptr[binder_at + 3 + i] = binder[i];
        tls_wipe(binder_key, sizeof binder_key);
#ifndef PROVEN_NO_HTTP3
        if (offer_early) {
            /* client_early_traffic_secret: the whole ClientHello, binders and all, under the
             * ticket's hash (RFC 8446 section 7.1). A first hello: nothing before it. */
            proven_tls_transcript_t ch;
            proven_tls_transcript_init(&ch, hash);
            proven_tls_transcript_update(&ch, (proven_mem_view_t){ .ptr = h->scratch.ptr, .size = h->scratch.len });
            (void)proven_tls_transcript_hash(&ch, th);
            proven_tls13_derive_secret(hash, early, "c e traffic", th, c->quic->early_secret);
            c->quic->early_suite = psk_suite;
            c->quic->early_ready = true;
            c->quic->early = PROVEN_TLS_EARLY_OFFERED;
        }
#endif
        if (tcp_early) {
            proven_tls_transcript_t ch;
            proven_tls_transcript_init(&ch, hash);
            proven_tls_transcript_update(&ch, (proven_mem_view_t){ .ptr = h->scratch.ptr, .size = h->scratch.len });
            (void)proven_tls_transcript_hash(&ch, th);
            proven_tls13_derive_secret(hash, early, "c e traffic", th, h->c_early);
            h->early_suite = psk_suite;
        }
        tls_wipe(early, sizeof early);
    }
    if (!h->transcript_ready && !buf_append(c, &h->first_hello, h->scratch.ptr, h->scratch.len)) return tls_nomem(c);
    /* The first record of a connection says version 3.1 for the benefit of old middleboxes. */
    if (h->transcript_ready) proven_tls_transcript_update(&h->transcript, (proven_mem_view_t){ .ptr = h->scratch.ptr, .size = h->scratch.len });
    if (!emit(c, PROVEN_TLS_CT_HANDSHAKE, h->scratch.ptr, h->scratch.len, h->retried ? 0x03 : 0x01)) return tls_nomem(c);
    if (tcp_early && h->psk_offered) {
        /* The compatibility ChangeCipherSpec goes before the first early record (RFC 8446
         * appendix D.4); then what is written is early data, under the early key. */
        if (!send_ccs(c)) return tls_nomem(c);
        keys_write(c, h->early_suite, h->c_early);
        c->early = PROVEN_TLS_EARLY_OFFERED;
        h->early_left = sess->early_max < 65536u ? sess->early_max : 65536u;
    }
    return PROVEN_OK;
}

static bool suite_was_offered(proven_u16 id) { return proven_tls_suite_find(id) != NULL; }
static proven_err_t client12_server_hello(proven_tls_conn_t *c, proven_u16 version, proven_rd_t random, proven_rd_t sid, proven_u16 suite_id, proven_rd_t exts, proven_mem_view_t whole);

static proven_err_t client_server_hello(proven_tls_conn_t *c, proven_rd_t body, proven_mem_view_t whole) {
    proven_tls_handshake_t *h = c->hs;
    proven_u16 version = (proven_u16)rd_uint(&body, 2);
    proven_rd_t random = rd_take(&body, 32);
    proven_rd_t sid = rd_vec(&body, 1);
    proven_u16 suite_id = (proven_u16)rd_uint(&body, 2);
    proven_u32 compression = rd_uint(&body, 1);
    /* A server from before extensions existed ends its hello here. */
    proven_rd_t exts = body.ok && body.n == 0 ? (proven_rd_t){ body.p, 0, true } : rd_vec(&body, 2);
    if (!body.ok || body.n != 0) return tls_decode(c);
    if (version < PROVEN_TLS_VERSION_1_0 || version > PROVEN_TLS_VERSION_1_2 || compression != 0) return tls_illegal(c);
    {
        /* A ServerHello without supported_versions is a TLS 1.2 server's. */
        proven_rd_t scan = exts;
        bool names_version = false;
        while (scan.n > 0) {
            proven_u16 type = (proven_u16)rd_uint(&scan, 2);
            (void)rd_vec(&scan, 2);
            if (!scan.ok) return tls_decode(c);
            if (type == EXT_SUPPORTED_VERSIONS) names_version = true;
        }
        /* Its version is then the number in the hello: one this side allows and offered. */
        if (!names_version && version >= c->config->min_version && version <= h->hello_version && !is_quic(c)) return client12_server_hello(c, version, random, sid, suite_id, exts, whole);
        if (c->config->max_version < PROVEN_TLS_VERSION_1_3 || !names_version) return tls_fail(c, AL_PROTOCOL_VERSION, PROVEN_ERR_PROTOCOL);
        if (version != PROVEN_TLS_VERSION_1_2) return tls_illegal(c);    /* TLS 1.3 keeps 3.3 here */
    }
    if (sid.n != h->session_id_len || !bytes_eq(sid.p, h->session_id, sid.n)) return tls_illegal(c);
    if (!suite_was_offered(suite_id)) return tls_illegal(c);
    bool retry = bytes_eq(random.p, HRR_RANDOM, 32);
    bool have_version = false, have_share = false, have_psk = false;
    proven_u16 group = 0;
    proven_rd_t share = { 0 };
    unsigned seen = 0;
    while (exts.n > 0) {
        proven_u16 type = (proven_u16)rd_uint(&exts, 2);
        proven_rd_t data = rd_vec(&exts, 2);
        if (!exts.ok) return tls_decode(c);
        unsigned bit = type == EXT_SUPPORTED_VERSIONS ? 1u : type == EXT_KEY_SHARE ? 2u : type == EXT_PRE_SHARED_KEY ? 4u : type == EXT_COOKIE ? 8u : 0u;
        if (bit == 0) return tls_fail(c, AL_UNSUPPORTED_EXTENSION, PROVEN_ERR_PROTOCOL);
        if (seen & bit) return tls_illegal(c);
        seen |= bit;
        if (type == EXT_SUPPORTED_VERSIONS) {
            if (data.n != 2 || rd_uint(&data, 2) != 0x0304) return tls_fail(c, AL_PROTOCOL_VERSION, PROVEN_ERR_PROTOCOL);
            have_version = true;
        } else if (type == EXT_KEY_SHARE) {
            group = (proven_u16)rd_uint(&data, 2);
            if (!retry) share = rd_vec(&data, 2);
            if (!data.ok || data.n != 0) return tls_decode(c);
            have_share = true;
        } else if (type == EXT_PRE_SHARED_KEY) {
            if (retry || data.n != 2) return tls_illegal(c);
            if (rd_uint(&data, 2) != 0 || !h->psk_offered) return tls_illegal(c);
            have_psk = true;
        } else {
            if (!retry) return tls_fail(c, AL_UNSUPPORTED_EXTENSION, PROVEN_ERR_PROTOCOL);
            proven_rd_t cookie = rd_vec(&data, 2);
            if (!data.ok || data.n != 0 || cookie.n == 0) return tls_decode(c);
            if (cookie.n > TLS_COOKIE_MAX) return tls_illegal(c);
            for (proven_size_t i = 0; i < cookie.n; ++i) h->cookie[i] = cookie.p[i];
            h->cookie_len = cookie.n;
        }
    }
    /* No supported_versions means a server that speaks only an older version. */
    if (!have_version) return tls_fail(c, AL_PROTOCOL_VERSION, PROVEN_ERR_PROTOCOL);
    const proven_tls_suite_t *suite = proven_tls_suite_find(suite_id);
    if (retry) {
        if (h->retried) return tls_unexpected(c);                 /* one retry, not two */
        /* It must ask for something else than what was sent, and something this side offered. */
        if (!have_share || group != GROUP_P256) return tls_illegal(c);
        c->suite = suite;
        proven_tls_transcript_init(&h->transcript, suite->hash);
        proven_tls_transcript_update(&h->transcript, (proven_mem_view_t){ .ptr = h->first_hello.ptr, .size = h->first_hello.len });
        proven_tls_transcript_retry(&h->transcript);
        tr_add(c, whole);
        h->transcript_ready = true;
        buf_free(c, &h->first_hello);
        h->retried = true;
        h->group = group;
        if (c->early == PROVEN_TLS_EARLY_OFFERED) {
            /* No early data after a retry (RFC 8446 section 4.1.2): what went early is not read,
             * and the second hello travels in the clear. */
            c->early = PROVEN_TLS_EARLY_REFUSED;
            tls_wipe(&c->live->write, sizeof c->live->write);
        }
        if (!send_ccs(c)) return tls_nomem(c);
        return client_hello_send(c);
    }
    if (!have_share) return tls_fail(c, AL_MISSING_EXTENSION, PROVEN_ERR_PROTOCOL);
    if (h->retried && suite != c->suite) return tls_illegal(c);
    if (group != (h->retried ? h->group : GROUP_X25519)) return tls_illegal(c);
    c->suite = suite;
    c->version = PROVEN_TLS_VERSION_1_3;
    h->group = group;
    if (have_psk) {
        /* The server may only select the key under a suite with the hash it was made for. */
        if (h->psk_len != suite->hash_len) return tls_illegal(c);
        h->psk_accepted = true;
        c->resumed = true;
        /* The server's identity is the one verified when the session was made. */
        proven_tls_session_data_t *s = (proven_tls_session_data_t *)(void *)c->session->opaque;
        c->has_peer_key = s->has_peer_key == 1;
        for (int i = 0; i < 32; ++i) c->peer_key_hash[i] = s->peer_key_hash[i];
        /* A pin is a statement about the key: it holds for a resumed session too. */
        if (c->config->pin_count > 0) {
            bool pinned = false;
            for (proven_size_t i = 0; c->has_peer_key && i < c->config->pin_count; ++i) pinned = pinned || bytes_eq(c->config->pins[i], c->peer_key_hash, 32);
            if (!pinned) { c->peer_fault = PROVEN_CERT_FAULT_PIN_MISMATCH; return tls_fail(c, AL_BAD_CERTIFICATE, PROVEN_ERR_UNTRUSTED); }
        }
    }
    if (!h->transcript_ready) {
        proven_tls_transcript_init(&h->transcript, suite->hash);
        proven_tls_transcript_update(&h->transcript, (proven_mem_view_t){ .ptr = h->first_hello.ptr, .size = h->first_hello.len });
        h->transcript_ready = true;
        buf_free(c, &h->first_hello);
    }
    tr_add(c, whole);
    proven_byte_t shared[32];
    if (!key_agree(c, group, (proven_mem_view_t){ .ptr = share.p, .size = share.n }, shared)) return tls_illegal(c);
    derive_handshake(c, shared, 32);
    tls_wipe(shared, sizeof shared);
    keys_read(c, c->suite, h->server_hs);
    if (c->early == PROVEN_TLS_EARLY_OFFERED && !have_psk) c->early = PROVEN_TLS_EARLY_REFUSED;   /* no ticket, no early data */
    /* From here on anything this side sends - an alert included - is under its handshake key;
     * with early data offered and the ticket taken, under the early key until EncryptedExtensions
     * says whether the server reads it (and then until EndOfEarlyData). */
    if (c->early != PROVEN_TLS_EARLY_OFFERED) keys_write(c, c->suite, h->client_hs);
    c->state = ST_C_WAIT_EE;
    return PROVEN_OK;
}

/* A client whose ClientHello offered early data, still waiting for the answer. */
static bool client_offered_early(const proven_tls_conn_t *c) {
#ifndef PROVEN_NO_HTTP3
    if (c->quic) return c->quic->early == PROVEN_TLS_EARLY_OFFERED;
#endif
    return c->early == PROVEN_TLS_EARLY_OFFERED;
}

static proven_err_t client_encrypted_extensions(proven_tls_conn_t *c, proven_rd_t body, proven_mem_view_t whole) {
    const proven_tls_config_t *cfg = c->config;
    proven_rd_t exts = rd_vec(&body, 2);
    if (!body.ok || body.n != 0) return tls_decode(c);
    unsigned seen = 0;
    while (exts.n > 0) {
        proven_u16 type = (proven_u16)rd_uint(&exts, 2);
        proven_rd_t data = rd_vec(&exts, 2);
        if (!exts.ok) return tls_decode(c);
        unsigned bit = type == EXT_ALPN ? 1u : type == EXT_SERVER_NAME ? 2u : type == EXT_RECORD_SIZE_LIMIT ? 4u : type == EXT_SUPPORTED_GROUPS ? 8u :
                       type == EXT_QUIC_TRANSPORT_PARAMETERS && is_quic(c) ? 16u : type == EXT_EARLY_DATA && client_offered_early(c) ? 32u : 0u;
        /* A server may only answer what was asked. */
        if (bit == 0) return tls_fail(c, AL_UNSUPPORTED_EXTENSION, PROVEN_ERR_PROTOCOL);
        if (seen & bit) return tls_illegal(c);
        seen |= bit;
        if (type == EXT_ALPN) {
            if (cfg->alpn_count == 0) return tls_fail(c, AL_UNSUPPORTED_EXTENSION, PROVEN_ERR_PROTOCOL);
            proven_rd_t list = rd_vec(&data, 2);
            proven_rd_t name = rd_vec(&list, 1);
            if (!data.ok || data.n != 0 || !list.ok || list.n != 0 || name.n == 0) return tls_decode(c);
            c->alpn_index = -1;
            for (proven_size_t i = 0; i < cfg->alpn_count; ++i) {
                proven_u8str_view_t ours = proven_tls_config_alpn(cfg, i);
                if (ours.size == name.n && bytes_eq(ours.ptr, name.p, name.n) && alpn_allowed(c, i)) c->alpn_index = (int)i;
            }
            if (c->alpn_index < 0) return tls_illegal(c);          /* a protocol that was not offered */
        } else if (type == EXT_RECORD_SIZE_LIMIT) {
            proven_u32 limit = rd_uint(&data, 2);
            if (!data.ok || data.n != 0 || limit < 64 || limit > PROVEN_TLS_MAX_PLAINTEXT + 1) return tls_illegal(c);
            c->peer_record_limit = limit - 1;                      /* the limit counts the content type byte */
        } else if (type == EXT_SERVER_NAME || type == EXT_EARLY_DATA) {
            if (data.n != 0) return tls_decode(c);
        }
#ifndef PROVEN_NO_HTTP3
        else if (type == EXT_QUIC_TRANSPORT_PARAMETERS) {
            proven_u64 err = proven_quic_tp_decode((proven_mem_view_t){ .ptr = data.p, .size = data.n }, true, &c->quic->tp_peer);
            if (err != 0) return quic_fail(c, err);
            c->quic->have_peer = true;
        }
#endif
    }
    /* QUIC: the server must name the protocol (RFC 9001 section 8.1) and send its parameters (section 8.2). */
    if (is_quic(c) && !(seen & 1u)) return tls_fail(c, AL_NO_APPLICATION_PROTOCOL, PROVEN_ERR_PROTOCOL);
    if (is_quic(c) && !(seen & 16u)) return tls_fail(c, AL_MISSING_EXTENSION, PROVEN_ERR_PROTOCOL);
    if (!is_quic(c) && c->early == PROVEN_TLS_EARLY_OFFERED) {
        /* Over TCP the same rule; refused, the handshake key from here on. */
        if (seen & 32u) {
            const proven_tls_session_data_t *s = (const proven_tls_session_data_t *)(const void *)c->session->opaque;
            const proven_u8str_view_t chosen = c->alpn_index >= 0 ? proven_tls_config_alpn(cfg, (proven_size_t)c->alpn_index) : (proven_u8str_view_t){ 0 };
            if (!c->hs->psk_accepted || chosen.size != s->alpn_len || !bytes_eq(chosen.ptr, s->alpn, s->alpn_len)) return tls_illegal(c);
            c->early = PROVEN_TLS_EARLY_ACCEPTED;
        } else {
            c->early = PROVEN_TLS_EARLY_REFUSED;
            keys_write(c, c->suite, c->hs->client_hs);
        }
    }
#ifndef PROVEN_NO_HTTP3
    if (is_quic(c) && client_offered_early(c)) {
        /* Accepted only with the ticket, and for the protocol the session was made for (RFC 8446
         * section 4.2.10): the early data assumed it. */
        if (seen & 32u) {
            const proven_tls_session_data_t *s = (const proven_tls_session_data_t *)(const void *)c->session->opaque;
            const proven_u8str_view_t chosen = c->alpn_index >= 0 ? proven_tls_config_alpn(cfg, (proven_size_t)c->alpn_index) : (proven_u8str_view_t){ 0 };
            if (!c->hs->psk_accepted || chosen.size != s->alpn_len || !bytes_eq(chosen.ptr, s->alpn, s->alpn_len)) return tls_illegal(c);
            c->quic->early = PROVEN_TLS_EARLY_ACCEPTED;
        } else {
            c->quic->early = PROVEN_TLS_EARLY_REFUSED;
        }
    }
#endif
    tr_add(c, whole);
    c->state = c->hs->psk_accepted ? ST_C_WAIT_FINISHED : ST_C_WAIT_CERT_CR;
    return PROVEN_OK;
}

static proven_err_t client_certificate_request(proven_tls_conn_t *c, proven_rd_t body, proven_mem_view_t whole) {
    proven_rd_t ctx = rd_vec(&body, 1);
    proven_rd_t exts = rd_vec(&body, 2);
    if (!body.ok || body.n != 0) return tls_decode(c);
    if (ctx.n != 0) return tls_illegal(c);
    bool have_algs = false, ours_ok = false;
    while (exts.n > 0) {
        proven_u16 type = (proven_u16)rd_uint(&exts, 2);
        proven_rd_t data = rd_vec(&exts, 2);
        if (!exts.ok) return tls_decode(c);
        if (type == EXT_SIGNATURE_ALGORITHMS) {
            proven_rd_t list = rd_vec(&data, 2);
            if (!data.ok || data.n != 0 || list.n == 0 || list.n % 2 != 0) return tls_decode(c);
            have_algs = true;
            ours_ok = c->config->key_kind != PROVEN_TLS_KEY_NONE && peer_accepts_our_key(c->config, list);
        }
    }
    if (!have_algs) return tls_fail(c, AL_MISSING_EXTENSION, PROVEN_ERR_PROTOCOL);
    c->hs->cert_requested = true;
    c->hs->peer_sig_schemes = ours_ok ? 1 : 0;
    tr_add(c, whole);
    c->state = ST_C_WAIT_CERT;
    return PROVEN_OK;
}

static proven_err_t client_store_ticket(proven_tls_conn_t *c, proven_rd_t body);

static proven_err_t client_finished(proven_tls_conn_t *c, proven_rd_t body, proven_mem_view_t whole) {
    proven_tls_handshake_t *h = c->hs;
    proven_byte_t client_ap[PROVEN_HMAC_MAX_SIZE], server_ap[PROVEN_HMAC_MAX_SIZE], th[PROVEN_HMAC_MAX_SIZE];
    proven_err_t e = finished_check(c, body, h->server_hs);
    if (e != PROVEN_OK) return e;
    tr_add(c, whole);
    derive_application(c, client_ap, server_ap);
    keys_read(c, c->suite, server_ap);
    if (!send_ccs(c)) return tls_nomem(c);
    if (c->early == PROVEN_TLS_EARLY_ACCEPTED && !is_quic(c)) {
        /* The end of the early data, under the early key and in the transcript (RFC 8446
         * section 4.5); then the handshake key for the rest of this flight. */
        proven_wr_t w = hs_begin(c, HS_END_OF_EARLY_DATA);
        if (!hs_end(&w) || !hs_send(c)) return tls_nomem(c);
        keys_write(c, c->suite, h->client_hs);
    }
    if (h->cert_requested) {
        bool send = h->peer_sig_schemes != 0;
        if (!cert_build(c, send) || !hs_send(c)) return tls_nomem(c);
        if (send && (!cv_build(c, false) || !hs_send(c))) return tls_nomem(c);
    }
    if (!finished_send(c, h->client_hs)) return tls_nomem(c);
    hs_hash(c, th);
    proven_tls13_derive_secret(c->suite->hash, h->master_secret, "res master", th, c->resumption_master);
    keys_write(c, c->suite, client_ap);
    establish(c, server_ap, client_ap);
    tls_wipe(client_ap, sizeof client_ap); tls_wipe(server_ap, sizeof server_ap);
    hs_free(c);
    return PROVEN_OK;
}

static proven_err_t client_message(proven_tls_conn_t *c, proven_byte_t type, proven_rd_t body, proven_mem_view_t whole, bool *keys_changed) {
    proven_err_t e;
    bool empty = false;
    switch (c->state) {
        case ST_C_WAIT_SH:
            if (type != HS_SERVER_HELLO) return tls_unexpected(c);
            e = client_server_hello(c, body, whole);
            *keys_changed = c->state == ST_C_WAIT_EE;
            return e;
        case ST_C_WAIT_EE:
            if (type != HS_ENCRYPTED_EXTENSIONS) return tls_unexpected(c);
            return client_encrypted_extensions(c, body, whole);
        case ST_C_WAIT_CERT_CR:
            if (type == HS_CERTIFICATE_REQUEST) return client_certificate_request(c, body, whole);
            /* fall through */
        case ST_C_WAIT_CERT:
            if (type != HS_CERTIFICATE) return tls_unexpected(c);
            e = cert_process(c, body, &empty);
            if (e != PROVEN_OK) return e;
            if (empty) return tls_decode(c);                      /* a server must present a certificate */
            tr_add(c, whole);
            c->state = ST_C_WAIT_CV;
            return PROVEN_OK;
        case ST_C_WAIT_CV:
            if (type != HS_CERTIFICATE_VERIFY) return tls_unexpected(c);
            e = cv_verify(c, body, true);
            if (e != PROVEN_OK) return e;
            tr_add(c, whole);
            c->state = ST_C_WAIT_FINISHED;
            return PROVEN_OK;
        case ST_C_WAIT_FINISHED:
            if (type != HS_FINISHED) return tls_unexpected(c);
            *keys_changed = true;
            return client_finished(c, body, whole);
        default:
            return tls_unexpected(c);
    }
}

/* ================= Server ================= */

static bool list_has_u16(proven_rd_t list, proven_u16 want) {
    while (list.n >= 2) if ((proven_u16)rd_uint(&list, 2) == want) return true;
    return false;
}

static proven_err_t server_send_ticket(proven_tls_conn_t *c);

#ifndef PROVEN_NO_HTTP3
/* A digest of the limits this side's transport parameters promise, which a client's 0-RTT
 * packets assume (RFC 9000 section 7.4.1): early data on a ticket is taken only while they are
 * what they were when the ticket was issued. */
static void limits_digest(const proven_tls_conn_t *c, proven_byte_t out[8]) {
    proven_quic_tp_t tp;
    proven_byte_t buf[7 * 8], d[32];
    for (int i = 0; i < 8; ++i) out[i] = 0;
    if (proven_quic_tp_decode((proven_mem_view_t){ .ptr = c->quic->tp_local, .size = c->quic->tp_local_len }, c->is_server, &tp) != 0) return;
    const proven_u64 v[7] = { tp.initial_max_data, tp.initial_max_stream_data_bidi_local, tp.initial_max_stream_data_bidi_remote,
                              tp.initial_max_stream_data_uni, tp.initial_max_streams_bidi, tp.initial_max_streams_uni, tp.active_connection_id_limit };
    for (int i = 0; i < 7; ++i) for (int k = 0; k < 8; ++k) buf[i * 8 + k] = (proven_byte_t)(v[i] >> (56 - 8 * k));
    proven_sha256((proven_mem_view_t){ .ptr = buf, .size = sizeof buf }, d);
    for (int i = 0; i < 8; ++i) out[i] = d[i];
}

#endif

#ifndef PROVEN_NO_HTTP3
/* Compatible version negotiation (RFC 9368 section 2.3; RFC-0036), before anything this side
 * sends is written: a server given a version to move to moves when the client's list has it,
 * and its own version_information says so. Everything else about the list is the QUIC layer's
 * to check. */
static void quic_upgrade(proven_tls_conn_t *c) {
    proven_tls_quic_t *q = c->quic;
    const proven_quic_tp_t *peer = &q->tp_peer;
    if (!q->upgrade_to || q->upgrade_to == q->version || !peer->has_version_info) return;
    bool listed = false;
    for (proven_size_t i = 0; i < peer->available_n; ++i) listed = listed || peer->available[i] == q->upgrade_to;
    proven_quic_tp_t mine;
    if (!listed || proven_quic_tp_decode((proven_mem_view_t){ .ptr = q->tp_local, .size = q->tp_local_len }, true, &mine) != 0) return;
    mine.has_version_info = true;
    mine.chosen_version = q->upgrade_to;
    const proven_size_t n = proven_quic_tp_encode(&mine, true, (proven_mem_mut_t){ .ptr = q->tp_local, .size = sizeof q->tp_local });
    if (n == 0) return;                           /* not reached: the same parameters fitted before */
    q->tp_local_len = n;
    q->version = q->upgrade_to;
}
#endif

/* Whether a server takes the early data of a ClientHello whose ticket it accepted. In QUIC the
 * ticket's limits must be today's; over TCP the ticket must have been made over TCP (it carries
 * no limits). The protocol must be the ticket's - over TCP, none for none. */
static bool server_early_ok(proven_tls_conn_t *c, const proven_tls_suite_t *suite, const proven_tls_ticket_state_t *t, const proven_byte_t id[16]) {
    const proven_tls_config_t *cfg = c->config;
    proven_tls_handshake_t *h = c->hs;
    if (!cfg->early_data || !h->psk_accepted || h->retried || !t->early) return false;
    if (t->suite != suite->id) return false;
    const bool no_alpn = c->alpn_index < 0 && t->alpn_index == 0xff && !is_quic(c);
    if (!no_alpn && (c->alpn_index < 0 || t->alpn_index != (proven_u8)c->alpn_index)) return false;
    proven_byte_t now_limits[8] = { 0 };
#ifndef PROVEN_NO_HTTP3
    if (is_quic(c)) limits_digest(c, now_limits);
#endif
    if (!bytes_eq(now_limits, t->limits, 8)) return false;
    /* Fresh: the age the client reports against the time since the ticket was issued. */
    const proven_i64 now = proven_tls_config_now(cfg);
    const proven_i64 said = (proven_i64)(proven_u32)(h->client_age - t->age_add), known = (now - t->issued_at) * 1000;
    const proven_i64 off = said > known ? said - known : known - said;
    if (off > (proven_i64)PROVEN_TLS_EARLY_WINDOW_S * 1000) return false;
    return proven_tls_early_take_(cfg, id, now);
}

static proven_err_t server12_client_hello(proven_tls_conn_t *c, proven_u16 version, proven_rd_t random, proven_rd_t sid, proven_rd_t suites, proven_rd_t exts, proven_rd_t groups, proven_rd_t sig_algs, proven_rd_t alpn, proven_mem_view_t whole);

static proven_err_t server_client_hello(proven_tls_conn_t *c, proven_rd_t body, proven_mem_view_t whole) {
    proven_tls_handshake_t *h = c->hs;
    const proven_tls_config_t *cfg = c->config;
    const proven_byte_t *msg_start = whole.ptr;
    proven_u16 version = (proven_u16)rd_uint(&body, 2);
    proven_rd_t random = rd_take(&body, 32);
    proven_rd_t sid = rd_vec(&body, 1);
    proven_rd_t suites = rd_vec(&body, 2);
    proven_rd_t compression = rd_vec(&body, 1);
    /* A client from before extensions existed ends its hello here. */
    proven_rd_t exts = body.ok && body.n == 0 ? (proven_rd_t){ body.p, 0, true } : rd_vec(&body, 2);
    if (!body.ok || body.n != 0) return tls_decode(c);
    /* Below TLS 1.0 there is nothing here. Whether the number is enough for this configuration
     * is decided further down: a client that lists its versions means the list, not this. */
    if (version < PROVEN_TLS_VERSION_1_0) return tls_fail(c, AL_PROTOCOL_VERSION, PROVEN_ERR_PROTOCOL);
    if (version > PROVEN_TLS_VERSION_1_2 || sid.n > 32 || suites.n < 2 || suites.n % 2 != 0) return tls_illegal(c);
    if (compression.n != 1 || compression.p[0] != 0) return tls_illegal(c);

    const proven_rd_t all_exts = exts;
    proven_rd_t versions = { 0 }, groups = { 0 }, shares = { 0 }, sig_algs = { 0 }, alpn = { 0 }, psk_ids = { 0 }, psk_binders = { 0 }, modes = { 0 }, quic_tp = { 0 };
    proven_size_t binders_at = 0;
    bool have_psk = false, have_sni = false;
    unsigned seen = 0;
    while (exts.n > 0) {
        proven_u16 type = (proven_u16)rd_uint(&exts, 2);
        proven_rd_t data = rd_vec(&exts, 2);
        if (!exts.ok) return tls_decode(c);
        if (have_psk) return tls_illegal(c);                       /* pre_shared_key must be the last extension */
        unsigned bit = 0;
        switch (type) {
            case EXT_SUPPORTED_VERSIONS: bit = 1; versions = rd_vec(&data, 1); break;
            case EXT_SUPPORTED_GROUPS: bit = 2; groups = rd_vec(&data, 2); break;
            case EXT_KEY_SHARE: bit = 4; shares = rd_vec(&data, 2); break;
            case EXT_SIGNATURE_ALGORITHMS: bit = 8; sig_algs = rd_vec(&data, 2); break;
            case EXT_ALPN: bit = 16; alpn = rd_vec(&data, 2); break;
            case EXT_PSK_MODES: bit = 32; modes = rd_vec(&data, 1); break;
            case EXT_SERVER_NAME: {
                bit = 64;
                proven_rd_t list = rd_vec(&data, 2);
                while (list.n > 0) {
                    proven_u32 kind = rd_uint(&list, 1);
                    proven_rd_t name = rd_vec(&list, 2);
                    if (!list.ok) return tls_decode(c);
                    if (kind == 0) {
                        if (have_sni || name.n == 0 || name.n > 253) return tls_illegal(c);
                        for (proven_size_t i = 0; i < name.n; ++i) h->sni[i] = name.p[i];
                        h->sni_len = name.n;
                        have_sni = true;
                    }
                }
                break; }
            case EXT_RECORD_SIZE_LIMIT: {
                bit = 128;
                proven_u32 limit = rd_uint(&data, 2);
                if (!data.ok || limit < 64 || limit > PROVEN_TLS_MAX_PLAINTEXT + 1) return tls_illegal(c);
                c->peer_record_limit = limit - 1;
                h->client_sent_record_limit = true;
                break; }
            case EXT_QUIC_TRANSPORT_PARAMETERS:
                /* Never sent where the transport is not QUIC, and refused there with a fatal
                 * unsupported_extension (RFC 9001 section 8.2). */
                if (!is_quic(c)) {
                    if (g_test_quic_tp_tcp) continue;             /* for a test: compare the key schedules */
                    return tls_fail(c, AL_UNSUPPORTED_EXTENSION, PROVEN_ERR_PROTOCOL);
                }
                bit = 512;
                quic_tp = data;
                data.n = 0;
                break;
            case EXT_PRE_SHARED_KEY:
                bit = 256;
                psk_ids = rd_vec(&data, 2);
                binders_at = (proven_size_t)(data.p - msg_start);
                psk_binders = rd_vec(&data, 2);
                have_psk = true;
                break;
            case EXT_EARLY_DATA:
                bit = 1024;
                h->client_early = true;
                /* Over TCP early data is never taken: what the client sends of it is passed
                 * over (RFC 8446 section 4.2.10), whatever else this hello leads to. */
                if (!is_quic(c) && !h->retried) h->early_skip = TLS_EARLY_SKIP;
                break;
            default:
                continue;                                         /* an extension this side does not use */
        }
        if (!data.ok || data.n != 0) return tls_decode(c);
        if (seen & bit) return tls_illegal(c);
        seen |= bit;
    }
    h->client_sent_sni = have_sni;
    h->client_sent_alpn = (seen & 16) != 0;
    {
        /* Which version. A client that knows 1.3 lists its versions; one that does not means
         * the number in the hello itself. This side takes the newest both allow. */
        bool listed = (seen & 1) != 0;
        if (listed && versions.n % 2 != 0) return tls_decode(c);
        bool client13 = listed && list_has_u16(versions, 0x0304);
        if (is_quic(c) && !client13) return tls_fail(c, AL_PROTOCOL_VERSION, PROVEN_ERR_PROTOCOL);
        if (!(client13 && cfg->max_version >= PROVEN_TLS_VERSION_1_3) || (g_test12 & 1u)) {
            /* Something older, then: the newest of 1.2, 1.1 and 1.0 that the client names -
             * in its list, or by the number in the hello, which means "up to this" - and
             * that this configuration allows. */
            proven_u16 agreed = 0;
            for (proven_u16 v = PROVEN_TLS_VERSION_1_2; v >= PROVEN_TLS_VERSION_1_0 && (agreed == 0 || (g_test12 & 64u)); --v) {
                const bool client_has = listed ? list_has_u16(versions, v) : v <= version;
                if (client_has && v >= cfg->min_version && v <= cfg->max_version) agreed = v;      /* (the knob: the oldest instead) */
            }
            if (agreed == 0 || h->retried) return tls_fail(c, AL_PROTOCOL_VERSION, PROVEN_ERR_PROTOCOL);
            return server12_client_hello(c, agreed, random, sid, suites, all_exts, groups, sig_algs, alpn, whole);
        }
        if (version != PROVEN_TLS_VERSION_1_2) return tls_illegal(c);       /* a TLS 1.3 hello keeps 3.3 here */
    }
    c->version = PROVEN_TLS_VERSION_1_3;
    if (!(seen & 2) || !(seen & 4) || groups.n % 2 != 0) return tls_fail(c, AL_MISSING_EXTENSION, PROVEN_ERR_PROTOCOL);
#ifndef PROVEN_NO_HTTP3
    if (c->quic) {
        /* RFC 9001 section 8.4: no middlebox compatibility mode in QUIC. Section 8.2: the
         * client's transport parameters, which must be there and well formed. */
        if (sid.n != 0) return quic_fail(c, PROVEN_QUIC_PROTOCOL_VIOLATION);
        if (!(seen & 512)) return tls_fail(c, AL_MISSING_EXTENSION, PROVEN_ERR_PROTOCOL);
        proven_u64 err = proven_quic_tp_decode((proven_mem_view_t){ .ptr = quic_tp.p, .size = quic_tp.n }, false, &c->quic->tp_peer);
        if (err != 0) return quic_fail(c, err);
        c->quic->have_peer = true;
        quic_upgrade(c);
    }
#else
    (void)quic_tp;
#endif

    /* The suite: the first of this side's order that the client offers. */
    proven_u16 order[3];
    const proven_tls_suite_t *suite = NULL;
    suite_order(order);
    for (int i = 0; i < 3 && !suite; ++i) if (list_has_u16(suites, order[i])) suite = proven_tls_suite_find(order[i]);
    if (!suite) return tls_fail(c, AL_HANDSHAKE_FAILURE, PROVEN_ERR_PROTOCOL);
    if (h->retried && suite != c->suite) return tls_illegal(c);     /* the second hello must still lead to the same choice */
    c->suite = suite;

    /* The key share: X25519 if the client sent one, then P-256. */
    proven_rd_t share_x = { 0 }, share_p = { 0 }, walk = shares;
    while (walk.n > 0) {
        proven_u16 g = (proven_u16)rd_uint(&walk, 2);
        proven_rd_t key = rd_vec(&walk, 2);
        if (!walk.ok) return tls_decode(c);
        if (g == GROUP_X25519 && !share_x.ok && !g_test_server_refuses_x25519) share_x = key;
        if (g == GROUP_P256 && !share_p.ok) share_p = key;
    }
    proven_u16 group = share_x.ok ? GROUP_X25519 : share_p.ok ? GROUP_P256 : 0;
    if (h->retried && group != h->group) return tls_illegal(c);
    if (group == 0) {
        /* Nothing usable was sent. If the client supports a group this side has, ask for it - once. */
        proven_u16 ask = list_has_u16(groups, GROUP_X25519) && !g_test_server_refuses_x25519 ? GROUP_X25519 : list_has_u16(groups, GROUP_P256) ? GROUP_P256 : 0;
        if (ask == 0) return tls_fail(c, AL_HANDSHAKE_FAILURE, PROVEN_ERR_PROTOCOL);
        if (h->retried) return tls_illegal(c);
        proven_tls_transcript_init(&h->transcript, suite->hash);
        tr_add(c, whole);
        proven_tls_transcript_retry(&h->transcript);
        h->transcript_ready = true;
        h->retried = true;
        h->group = ask;
        h->session_id_len = sid.n;
        for (proven_size_t i = 0; i < sid.n; ++i) h->session_id[i] = sid.p[i];
        proven_wr_t w = hs_begin(c, HS_SERVER_HELLO);
        wr_uint(&w, 0x0303, 2); wr_bytes(&w, HRR_RANDOM, 32);
        wr_uint(&w, (proven_u32)sid.n, 1); wr_bytes(&w, sid.p, sid.n);
        wr_uint(&w, suite->id, 2); wr_uint(&w, 0, 1);
        proven_size_t e = wr_open(&w, 2);
        wr_uint(&w, EXT_SUPPORTED_VERSIONS, 2); wr_uint(&w, 2, 2); wr_uint(&w, 0x0304, 2);
        wr_uint(&w, EXT_KEY_SHARE, 2); wr_uint(&w, 2, 2); wr_uint(&w, ask, 2);
        wr_close(&w, e, 2);
        if (!hs_end(&w) || !hs_send(c)) return tls_nomem(c);
        if (!send_ccs(c)) return tls_nomem(c);
        return PROVEN_OK;                                           /* still waiting for a ClientHello */
    }
    if (h->retried && (sid.n != h->session_id_len || !bytes_eq(sid.p, h->session_id, sid.n))) return tls_illegal(c);
    if (!h->transcript_ready) { proven_tls_transcript_init(&h->transcript, suite->hash); h->transcript_ready = true; }
    h->group = group;
    h->session_id_len = sid.n;
    for (proven_size_t i = 0; i < sid.n; ++i) h->session_id[i] = sid.p[i];

    /* Resumption: the first identity, if it is a ticket of this server, made for a suite with
     * this hash, and its binder proves the client holds the key. The binder is checked before
     * the key is used for anything else. */
    proven_tls_ticket_state_t ticket = { 0 }, early_ticket = { 0 };
    proven_byte_t early_id[16] = { 0 };
    if (have_psk) {
        proven_rd_t ids = psk_ids, binders = psk_binders;
        proven_rd_t identity = rd_vec(&ids, 2);
        h->client_age = rd_uint(&ids, 4);
        proven_rd_t binder = rd_vec(&binders, 1);
        if (!ids.ok || !binders.ok || identity.n == 0 || binder.n < 32) return tls_decode(c);
        bool usable = !cfg->no_resumption && modes.ok && modes.n > 0;
        bool dhe = false;
        for (proven_size_t i = 0; usable && i < modes.n; ++i) dhe = dhe || modes.p[i] == 1;
        if (usable && dhe && proven_tls_ticket_open(cfg, (proven_mem_view_t){ .ptr = identity.p, .size = identity.n }, &ticket)) {
            const proven_tls_suite_t *made = proven_tls_suite_find(ticket.suite);
            /* A ticket of the other QUIC version is not taken (RFC 9369 section 5): a full handshake. */
#ifndef PROVEN_NO_HTTP3
            const bool v2_now = is_quic(c) && c->quic->orig_version == PROVEN_QUIC_VERSION_2;
#else
            const bool v2_now = false;
#endif
            if (made && made->hash == suite->hash && ticket.psk_len == suite->hash_len && ticket.quic_v2 == v2_now) {
                proven_tls_transcript_t t = h->transcript, empty;
                proven_byte_t early[PROVEN_HMAC_MAX_SIZE], key[PROVEN_HMAC_MAX_SIZE], th[PROVEN_HMAC_MAX_SIZE], want[PROVEN_HMAC_MAX_SIZE];
                proven_tls_transcript_update(&t, (proven_mem_view_t){ .ptr = msg_start, .size = binders_at });
                (void)proven_tls_transcript_hash(&t, th);
                proven_tls_transcript_init(&empty, suite->hash);
                (void)proven_tls_transcript_hash(&empty, want);
                proven_tls13_extract(suite->hash, NULL, (proven_mem_view_t){ .ptr = ticket.psk, .size = ticket.psk_len }, early);
                proven_tls13_derive_secret(suite->hash, early, "res binder", want, key);
                proven_tls13_finished(suite->hash, key, th, want);
                bool match = binder.n == suite->hash_len &&
                             proven_mem_equal_ct((proven_mem_view_t){ .ptr = want, .size = binder.n }, (proven_mem_view_t){ .ptr = binder.p, .size = binder.n });
                tls_wipe(early, sizeof early); tls_wipe(key, sizeof key);
                if (!match) return tls_fail(c, AL_DECRYPT_ERROR, PROVEN_ERR_PROTOCOL);
                h->psk_accepted = true;
                h->psk_len = ticket.psk_len;
                for (proven_size_t i = 0; i < ticket.psk_len; ++i) h->psk[i] = ticket.psk[i];
                c->resumed = true;
                c->has_peer_key = ticket.has_peer_key;
                for (int i = 0; i < 32; ++i) c->peer_key_hash[i] = ticket.peer_key_hash[i];
                early_ticket = ticket;
                for (int i = 0; i < 16; ++i) early_id[i] = identity.p[i];
            }
        }
        tls_wipe(&ticket, sizeof ticket);
    }
    if (!h->psk_accepted) {
        if (!(seen & 8) || sig_algs.n % 2 != 0) return tls_fail(c, AL_MISSING_EXTENSION, PROVEN_ERR_PROTOCOL);
        if (!peer_accepts_our_key(cfg, sig_algs)) return tls_fail(c, AL_HANDSHAKE_FAILURE, PROVEN_ERR_PROTOCOL);
    }
    /* ALPN: this side's first choice among what the client offers; no overlap is a failure. */
    c->alpn_index = -1;
    /* QUIC: a protocol must be agreed (RFC 9001 section 8.1). */
    if (is_quic(c) && (!h->client_sent_alpn || cfg->alpn_count == 0)) return tls_fail(c, AL_NO_APPLICATION_PROTOCOL, PROVEN_ERR_PROTOCOL);
    if (h->client_sent_alpn && cfg->alpn_count > 0) {
        for (proven_size_t i = 0; i < cfg->alpn_count && c->alpn_index < 0; ++i) {
            proven_u8str_view_t ours = proven_tls_config_alpn(cfg, i);
            proven_rd_t list = alpn;
            while (list.n > 0) {
                proven_rd_t name = rd_vec(&list, 1);
                if (!list.ok || name.n == 0) return tls_decode(c);
                if (name.n == ours.size && bytes_eq(name.p, ours.ptr, name.n) && alpn_allowed(c, i)) { c->alpn_index = (int)i; break; }
            }
        }
        if (c->alpn_index < 0) return tls_fail(c, AL_NO_APPLICATION_PROTOCOL, PROVEN_ERR_PROTOCOL);
    }
    tr_add(c, whole);
#ifndef PROVEN_NO_HTTP3
    /* Early data (RFC-0021): taken only when every condition of RFC-0021 10.1 P-5 holds; the
     * register last, so that a ticket is marked used only when the rest would have let it in. */
    if (is_quic(c)) {
        c->quic->early = h->client_early ? PROVEN_TLS_EARLY_REFUSED : PROVEN_TLS_EARLY_NONE;
        if (h->client_early && server_early_ok(c, suite, &early_ticket, early_id)) {
            proven_byte_t es[PROVEN_HMAC_MAX_SIZE], th[PROVEN_HMAC_MAX_SIZE];
            proven_tls13_extract(suite->hash, NULL, (proven_mem_view_t){ .ptr = h->psk, .size = h->psk_len }, es);
            hs_hash(c, th);
            proven_tls13_derive_secret(suite->hash, es, "c e traffic", th, c->quic->early_secret);
            tls_wipe(es, sizeof es);
            c->quic->early_suite = suite;
            c->quic->early_ready = true;
            c->quic->early = PROVEN_TLS_EARLY_ACCEPTED;
        }
    }
#endif
    /* Over TCP (RFC-0027): only where the owner takes early data; the same rules. */
    if (!is_quic(c) && h->client_early) {
        c->early = PROVEN_TLS_EARLY_REFUSED;
        if (c->take_early && server_early_ok(c, suite, &early_ticket, early_id)) {
            proven_byte_t es[PROVEN_HMAC_MAX_SIZE], th[PROVEN_HMAC_MAX_SIZE];
            proven_tls13_extract(suite->hash, NULL, (proven_mem_view_t){ .ptr = h->psk, .size = h->psk_len }, es);
            hs_hash(c, th);
            proven_tls13_derive_secret(suite->hash, es, "c e traffic", th, h->c_early);
            tls_wipe(es, sizeof es);
            h->early_suite = suite;
            h->early_left = PROVEN_TLS_EARLY_TCP_MAX;
            h->early_skip = 0;                                     /* read, not passed over */
            c->early = PROVEN_TLS_EARLY_ACCEPTED;
        }
    }
    tls_wipe(&early_ticket, sizeof early_ticket);

    /* This side's share and the key both arrive at. */
    proven_byte_t shared[32], pub[65];
    proven_size_t pub_len;
    proven_rd_t theirs = group == GROUP_X25519 ? share_x : share_p;
    if (group == GROUP_X25519) {
        proven_tls_config_random(cfg, h->x25519_priv, 32);
        proven_crypto_x25519_public(pub, h->x25519_priv);
        pub_len = 32;
    } else {
        make_p256_key(c, pub);
        pub_len = 65;
    }
    if (!key_agree(c, group, (proven_mem_view_t){ .ptr = theirs.p, .size = theirs.n }, shared)) return tls_illegal(c);

    proven_byte_t server_random[32];
    proven_tls_config_random(cfg, server_random, 32);
    proven_wr_t w = hs_begin(c, HS_SERVER_HELLO);
    wr_uint(&w, 0x0303, 2); wr_bytes(&w, server_random, 32);
    wr_uint(&w, (proven_u32)sid.n, 1); wr_bytes(&w, h->session_id, sid.n);
    wr_uint(&w, suite->id, 2); wr_uint(&w, 0, 1);
    proven_size_t e = wr_open(&w, 2);
    wr_uint(&w, EXT_SUPPORTED_VERSIONS, 2); wr_uint(&w, 2, 2); wr_uint(&w, 0x0304, 2);
    wr_uint(&w, EXT_KEY_SHARE, 2); wr_uint(&w, (proven_u32)pub_len + 4, 2); wr_uint(&w, group, 2); wr_uint(&w, (proven_u32)pub_len, 2); wr_bytes(&w, pub, pub_len);
    if (h->psk_accepted) { wr_uint(&w, EXT_PRE_SHARED_KEY, 2); wr_uint(&w, 2, 2); wr_uint(&w, 0, 2); }
    wr_close(&w, e, 2);
    if (!hs_end(&w) || !hs_send(c)) return tls_nomem(c);
    if (!send_ccs(c)) return tls_nomem(c);
    derive_handshake(c, shared, 32);
    tls_wipe(shared, sizeof shared);
    keys_write(c, suite, h->server_hs);
    /* Taken early data comes first, under the early key, until EndOfEarlyData. */
    keys_read(c, suite, c->early == PROVEN_TLS_EARLY_ACCEPTED ? h->c_early : h->client_hs);

    w = hs_begin(c, HS_ENCRYPTED_EXTENSIONS);
    e = wr_open(&w, 2);
    if (c->alpn_index >= 0) {
        proven_u8str_view_t name = proven_tls_config_alpn(cfg, (proven_size_t)c->alpn_index);
        wr_uint(&w, EXT_ALPN, 2); wr_uint(&w, (proven_u32)name.size + 3, 2); wr_uint(&w, (proven_u32)name.size + 1, 2);
        wr_uint(&w, (proven_u32)name.size, 1); wr_bytes(&w, name.ptr, name.size);
    }
    if (h->client_sent_sni) { wr_uint(&w, EXT_SERVER_NAME, 2); wr_uint(&w, 0, 2); }
    if (h->client_sent_record_limit && !is_quic(c)) { wr_uint(&w, EXT_RECORD_SIZE_LIMIT, 2); wr_uint(&w, 2, 2); wr_uint(&w, PROVEN_TLS_MAX_PLAINTEXT + 1, 2); }
    if (!is_quic(c) && c->early == PROVEN_TLS_EARLY_ACCEPTED) { wr_uint(&w, EXT_EARLY_DATA, 2); wr_uint(&w, 0, 2); }
#ifndef PROVEN_NO_HTTP3
    if (c->quic) {
        wr_uint(&w, EXT_QUIC_TRANSPORT_PARAMETERS, 2); wr_uint(&w, (proven_u32)c->quic->tp_local_len, 2);
        wr_bytes(&w, c->quic->tp_local, c->quic->tp_local_len);
        if (c->quic->early == PROVEN_TLS_EARLY_ACCEPTED) { wr_uint(&w, EXT_EARLY_DATA, 2); wr_uint(&w, 0, 2); }
    }
#endif
    wr_close(&w, e, 2);
    if (!hs_end(&w) || !hs_send(c)) return tls_nomem(c);

    bool ask_cert = !h->psk_accepted && cfg->client_auth != PROVEN_TLS_CLIENT_AUTH_NONE;
    if (!h->psk_accepted) {
        if (ask_cert) {
            w = hs_begin(c, HS_CERTIFICATE_REQUEST);
            wr_uint(&w, 0, 1);
            e = wr_open(&w, 2);
            wr_sig_algs(&w);
            wr_close(&w, e, 2);
            if (!hs_end(&w) || !hs_send(c)) return tls_nomem(c);
        }
        if (!cert_build(c, true) || !hs_send(c)) return tls_nomem(c);
        if (!cv_build(c, true) || !hs_send(c)) return tls_nomem(c);
    }
    if (!finished_send(c, h->server_hs)) return tls_nomem(c);
    /* The server may write application data from here; this side's sending keys change now. */
    proven_byte_t client_ap[PROVEN_HMAC_MAX_SIZE], server_ap[PROVEN_HMAC_MAX_SIZE];
    derive_application(c, client_ap, server_ap);
    keys_write(c, suite, server_ap);
    for (proven_size_t i = 0; i < suite->hash_len; ++i) { c->write_secret[i] = server_ap[i]; c->read_secret[i] = client_ap[i]; }
    tls_wipe(client_ap, sizeof client_ap); tls_wipe(server_ap, sizeof server_ap);
    c->state = c->early == PROVEN_TLS_EARLY_ACCEPTED ? ST_S_WAIT_EOED : ask_cert ? ST_S_WAIT_CERT : ST_S_WAIT_FINISHED;
    /* The ticket with this flight, not after the client's Finished (RFC 8446 section 4.6.1): with
     * no client certificate asked for, the rest of the transcript is known - EndOfEarlyData when
     * early data was taken, and the Finished the client must send - and so is the resumption
     * secret. A client that reads its answer and closes at once still leaves with a ticket. The
     * client's Finished is checked as before when it comes. Over TCP. */
    if (!is_quic(c) && !ask_cert && !cfg->no_resumption) {
        proven_tls_transcript_t t = h->transcript;
        proven_byte_t th[PROVEN_HMAC_MAX_SIZE], fin[4 + PROVEN_HMAC_MAX_SIZE];
        if (c->early == PROVEN_TLS_EARLY_ACCEPTED) {
            static const proven_byte_t eoed[4] = { HS_END_OF_EARLY_DATA, 0, 0, 0 };
            proven_tls_transcript_update(&t, (proven_mem_view_t){ .ptr = eoed, .size = 4 });
        }
        (void)proven_tls_transcript_hash(&t, th);
        fin[0] = HS_FINISHED; fin[1] = 0; fin[2] = 0; fin[3] = (proven_byte_t)suite->hash_len;
        proven_tls13_finished(suite->hash, h->client_hs, th, fin + 4);
        proven_tls_transcript_update(&t, (proven_mem_view_t){ .ptr = fin, .size = 4 + suite->hash_len });
        (void)proven_tls_transcript_hash(&t, th);
        proven_tls13_derive_secret(suite->hash, h->master_secret, "res master", th, c->resumption_master);
        tls_wipe(fin, sizeof fin);
        tls_wipe(&t, sizeof t);
        proven_err_t te = server_send_ticket(c);
        if (te != PROVEN_OK) return te;
        h->ticket_sent = true;
    }
    return PROVEN_OK;
}

static proven_err_t server_finished(proven_tls_conn_t *c, proven_rd_t body, proven_mem_view_t whole) {
    proven_tls_handshake_t *h = c->hs;
    proven_byte_t th[PROVEN_HMAC_MAX_SIZE];
    proven_err_t e = finished_check(c, body, h->client_hs);
    if (e != PROVEN_OK) return e;
    tr_add(c, whole);
    hs_hash(c, th);
    proven_tls13_derive_secret(c->suite->hash, h->master_secret, "res master", th, c->resumption_master);
    keys_read(c, c->suite, c->read_secret);
    c->state = ST_ESTABLISHED;
    c->established = true;
    e = c->config->no_resumption || h->ticket_sent ? PROVEN_OK : server_send_ticket(c);
    hs_free(c);
    return e;
}

static proven_err_t server_send_ticket(proven_tls_conn_t *c) {
    const proven_tls_config_t *cfg = c->config;
    proven_tls_ticket_state_t st = { 0 };
    proven_byte_t ticket[PROVEN_TLS_TICKET_MAX], nonce = 0, age_add[4];
    st.suite = c->suite->id;
    st.psk_len = c->suite->hash_len;
    proven_tls13_expand_label(c->suite->hash, c->resumption_master, "resumption", (proven_mem_view_t){ .ptr = &nonce, .size = 1 }, st.psk, st.psk_len);
    st.issued_at = proven_tls_config_now(cfg);
    st.has_peer_key = c->has_peer_key;
    for (int i = 0; i < 32; ++i) st.peer_key_hash[i] = c->peer_key_hash[i];
    proven_tls_config_random(cfg, age_add, 4);
    st.age_add = ((proven_u32)age_add[0] << 24) | ((proven_u32)age_add[1] << 16) | ((proven_u32)age_add[2] << 8) | age_add[3];
    st.alpn_index = c->alpn_index >= 0 ? (proven_u8)c->alpn_index : 0xff;
#ifndef PROVEN_NO_HTTP3
    st.quic_v2 = is_quic(c) && c->quic->version == PROVEN_QUIC_VERSION_2;
#endif
    /* Early data: under a configuration that takes it - in QUIC with a protocol agreed, and the
     * limits the 0-RTT packets will assume; over TCP where this connection's owner takes it
     * (RFC-0027), with no limits recorded, which is what marks the ticket as made over TCP. */
#ifndef PROVEN_NO_HTTP3
    if (is_quic(c)) {
        st.early = cfg->early_data && c->alpn_index >= 0;
        if (st.early) limits_digest(c, st.limits);
    } else
#endif
    st.early = cfg->early_data && c->take_early;
    proven_size_t ticket_len = proven_tls_ticket_seal(cfg, &st, ticket);
    const bool early = st.early;
    tls_wipe(&st, sizeof st);
    proven_wr_t w = hs_begin(c, HS_NEW_SESSION_TICKET);
    wr_uint(&w, cfg->ticket_lifetime_s, 4);
    wr_bytes(&w, age_add, 4);
    wr_uint(&w, 1, 1); wr_uint(&w, nonce, 1);
    wr_uint(&w, (proven_u32)ticket_len, 2); wr_bytes(&w, ticket, ticket_len);
    if (early) { wr_uint(&w, 8, 2); wr_uint(&w, EXT_EARLY_DATA, 2); wr_uint(&w, 4, 2); wr_uint(&w, is_quic(c) ? 0xffffffffu : (proven_u32)PROVEN_TLS_EARLY_TCP_MAX, 4); }
    else wr_uint(&w, 0, 2);
    /* Sent after the handshake: it is not part of the transcript. */
    if (!hs_end(&w) || !emit(c, PROVEN_TLS_CT_HANDSHAKE, w.b->ptr, w.b->len, 0x03)) return tls_nomem(c);
    return PROVEN_OK;
}

static proven_err_t server_message(proven_tls_conn_t *c, proven_byte_t type, proven_rd_t body, proven_mem_view_t whole, bool *keys_changed) {
    proven_err_t e;
    bool empty = false;
    switch (c->state) {
        case ST_S_WAIT_CH:
            if (type != HS_CLIENT_HELLO) return tls_unexpected(c);
            e = server_client_hello(c, body, whole);
            *keys_changed = c->state != ST_S_WAIT_CH;
            return e;
        case ST_S_WAIT_EOED:
            if (type != HS_END_OF_EARLY_DATA) return tls_unexpected(c);
            if (body.n != 0) return tls_decode(c);
            tr_add(c, whole);
            keys_read(c, c->suite, c->hs->client_hs);
            *keys_changed = true;
            c->state = ST_S_WAIT_FINISHED;
            return PROVEN_OK;
        case ST_S_WAIT_CERT:
            if (type != HS_CERTIFICATE) return tls_unexpected(c);
            e = cert_process(c, body, &empty);
            if (e != PROVEN_OK) return e;
            if (empty) {
                if (c->config->client_auth == PROVEN_TLS_CLIENT_AUTH_REQUIRE) return tls_fail(c, AL_CERTIFICATE_REQUIRED, PROVEN_ERR_UNTRUSTED);
                tr_add(c, whole);
                c->state = ST_S_WAIT_FINISHED;
                return PROVEN_OK;
            }
            tr_add(c, whole);
            c->state = ST_S_WAIT_CV;
            return PROVEN_OK;
        case ST_S_WAIT_CV:
            if (type != HS_CERTIFICATE_VERIFY) return tls_unexpected(c);
            e = cv_verify(c, body, false);
            if (e != PROVEN_OK) return e;
            tr_add(c, whole);
            c->state = ST_S_WAIT_FINISHED;
            return PROVEN_OK;
        case ST_S_WAIT_FINISHED:
            if (type != HS_FINISHED) return tls_unexpected(c);
            *keys_changed = true;
            return server_finished(c, body, whole);
        default:
            return tls_unexpected(c);
    }
}

/* ================= After the handshake ================= */

static proven_err_t client_store_ticket(proven_tls_conn_t *c, proven_rd_t body) {
    proven_u32 lifetime = rd_uint(&body, 4), age_add = rd_uint(&body, 4);
    proven_rd_t nonce = rd_vec(&body, 1);
    proven_rd_t ticket = rd_vec(&body, 2);
    proven_rd_t exts = rd_vec(&body, 2);
    /* early_data says the ticket allows it; in QUIC its value must be 0xffffffff (RFC 9001
     * section 4.6.1), and anything else ends the connection. */
    bool early = false;
    proven_u32 early_max = 0;
    while (body.ok && exts.ok && exts.n > 0) {
        proven_u16 type = (proven_u16)rd_uint(&exts, 2);
        proven_rd_t data = rd_vec(&exts, 2);
        if (type != EXT_EARLY_DATA || !exts.ok) continue;
        proven_u32 max = rd_uint(&data, 4);
        if (!data.ok || data.n != 0) return tls_decode(c);
        early_max = max;
#ifndef PROVEN_NO_HTTP3
        if (is_quic(c) && max != 0xffffffffu) return quic_fail(c, PROVEN_QUIC_PROTOCOL_VIOLATION);
#endif
        early = max > 0;
    }
    /* A ticket that is malformed or does not fit is simply not kept: resumption is optional. */
    if (!body.ok || !exts.ok || !c->session || c->config->no_resumption || ticket.n == 0 || ticket.n > PROVEN_TLS_SESSION_TICKET_MAX || lifetime == 0) return PROVEN_OK;
    proven_tls_session_data_t *s = (proven_tls_session_data_t *)(void *)c->session->opaque;
    /* The NEW_TOKEN token is the QUIC layer's and outlives tickets: kept across the wipe. */
    proven_u16 token_len = s->token_len <= PROVEN_TLS_SESSION_TOKEN_MAX ? s->token_len : 0;
    proven_byte_t token[PROVEN_TLS_SESSION_TOKEN_MAX];
    for (proven_size_t i = 0; i < token_len; ++i) token[i] = s->token[i];
    const proven_byte_t token_v2 = s->magic == PROVEN_TLS_SESSION_MAGIC ? s->token_v2 : 0;
    tls_wipe(s, sizeof *s);
    s->token_len = token_len;
    for (proven_size_t i = 0; i < token_len; ++i) s->token[i] = token[i];
    s->token_v2 = token_v2;
#ifndef PROVEN_NO_HTTP3
    s->ticket_v2 = is_quic(c) && c->quic->version == PROVEN_QUIC_VERSION_2 ? 1 : 0;
#endif
    s->suite = c->suite->id;
    s->psk_len = (proven_u16)c->suite->hash_len;
    proven_tls13_expand_label(c->suite->hash, c->resumption_master, "resumption", (proven_mem_view_t){ .ptr = nonce.p, .size = nonce.n }, s->psk, s->psk_len);
    s->received_at = proven_tls_config_now(c->config);
    s->lifetime_s = lifetime > 604800 ? 604800 : lifetime;
    s->age_add = age_add;
    s->name_len = (proven_u16)c->server_name_len;
    for (proven_size_t i = 0; i < c->server_name_len; ++i) s->name[i] = c->server_name[i];
    s->has_peer_key = c->has_peer_key ? 1 : 0;
    for (int i = 0; i < 32; ++i) s->peer_key_hash[i] = c->peer_key_hash[i];
    s->ticket_len = (proven_u16)ticket.n;
    for (proven_size_t i = 0; i < ticket.n; ++i) s->ticket[i] = ticket.p[i];
    if (c->alpn_index >= 0) {
        proven_u8str_view_t name = proven_tls_config_alpn(c->config, (proven_size_t)c->alpn_index);
        s->alpn_len = (proven_byte_t)name.size;
        for (proven_size_t i = 0; i < name.size; ++i) s->alpn[i] = name.ptr[i];
    }
#ifndef PROVEN_NO_HTTP3
    /* Early data needs the server's limits, which the next connection's 0-RTT packets assume. */
    if (is_quic(c) && c->quic->have_peer) {
        const proven_quic_tp_t *tp = &c->quic->tp_peer;
        const proven_u64 v[PROVEN_TLS_SESSION_LIMITS] = { tp->initial_max_data, tp->initial_max_stream_data_bidi_local, tp->initial_max_stream_data_bidi_remote,
                                                          tp->initial_max_stream_data_uni, tp->initial_max_streams_bidi, tp->initial_max_streams_uni, tp->active_connection_id_limit };
        for (int i = 0; i < PROVEN_TLS_SESSION_LIMITS; ++i) s->limits[i] = v[i];
        s->has_limits = 1;
        s->early = early && s->alpn_len > 0 ? 1 : 0;
    }
#endif
    /* Over TCP: what the ticket allows, kept for the next connection (RFC-0027). */
    if (!is_quic(c) && early) { s->early = 1; s->early_max = early_max; }
    s->magic = PROVEN_TLS_SESSION_MAGIC;
    c->ticket_stored = true;
    return PROVEN_OK;
}

/* The next traffic secret of a direction (RFC 8446 section 7.2). */
static void rotate(proven_tls_conn_t *c, proven_byte_t *secret, proven_tls_keys_t *keys) {
    proven_byte_t next[PROVEN_HMAC_MAX_SIZE];
    proven_tls13_expand_label(c->suite->hash, secret, "traffic upd", (proven_mem_view_t){ .ptr = next, .size = 0 }, next, c->suite->hash_len);
    for (proven_size_t i = 0; i < c->suite->hash_len; ++i) secret[i] = next[i];
    proven_tls13_set_keys(keys, c->suite, secret);
    tls_wipe(next, sizeof next);
}

static bool key_update_send(proven_tls_conn_t *c, proven_byte_t request) {
    proven_byte_t msg[5] = { HS_KEY_UPDATE, 0, 0, 1, request };
    if (!emit(c, PROVEN_TLS_CT_HANDSHAKE, msg, 5, 0x03)) return false;
    rotate(c, c->write_secret, &c->live->write);
    return true;
}

static proven_err_t post_message(proven_tls_conn_t *c, proven_byte_t type, proven_rd_t body, bool *keys_changed) {
    if (type == HS_NEW_SESSION_TICKET) {
        if (c->is_server) return tls_unexpected(c);
        return client_store_ticket(c, body);
    }
    if (type == HS_KEY_UPDATE) {
        /* QUIC changes keys with its own key phase bit; the message is refused (RFC 9001 section 6). */
        if (is_quic(c)) return tls_unexpected(c);
        if (body.n != 1 || body.p[0] > 1) return tls_illegal(c);
        rotate(c, c->read_secret, &c->live->read);
        *keys_changed = true;
        if (body.p[0] == 1 && !c->close_sent) {
            /* Each request is owed an answer, and answers queue until the caller sends them. A
             * peer that asks faster than it reads would make that queue grow without limit. */
            if (c->out.len - c->out_off > TLS_MAX_UNSENT) return tls_unexpected(c);
            if (!key_update_send(c, 0)) return tls_nomem(c);
        }
        return PROVEN_OK;
    }
    return tls_unexpected(c);
}

/* ================= Records in ================= */

/* Handshake bytes from one record. Messages may span records, and several may share one; a
 * message that changes the reading keys must be the last thing in its record. */
static proven_err_t client12_message(proven_tls_conn_t *c, proven_byte_t type, proven_rd_t body, proven_mem_view_t whole);
static proven_err_t server12_message(proven_tls_conn_t *c, proven_byte_t type, proven_rd_t body, proven_mem_view_t whole);
static proven_err_t post12_message(proven_tls_conn_t *c, proven_byte_t type);

static proven_err_t handshake_bytes(proven_tls_conn_t *c, const proven_byte_t *data, proven_size_t len) {
    proven_tls_buf_t *acc = c->established ? &c->post : &c->hs->reassembly;
    proven_size_t limit = c->config->max_handshake_bytes;
    if (len == 0) return tls_unexpected(c);
    if (acc->len + len > limit + 4) return tls_fail(c, AL_ILLEGAL_PARAMETER, PROVEN_ERR_PROTOCOL);
    if (!buf_append(c, acc, data, len)) return tls_nomem(c);
    proven_size_t at = 0;
    while (acc->len - at >= 4) {
        const proven_byte_t *p = acc->ptr + at;
        proven_size_t mlen = ((proven_size_t)p[1] << 16) | ((proven_size_t)p[2] << 8) | p[3];
        if (mlen > limit) return tls_fail(c, AL_ILLEGAL_PARAMETER, PROVEN_ERR_PROTOCOL);
        if (acc->len - at < 4 + mlen) break;
        proven_rd_t body = { p + 4, mlen, true };
        proven_mem_view_t whole = { .ptr = p, .size = 4 + mlen };
        bool keys_changed = false, was_established = c->established;
        proven_size_t total = acc->len;
        /* Which machine the message belongs to. A connection becomes TLS 1.2 inside the first
         * hello it processes, so the hellos themselves always go to the first two. */
        proven_err_t e = c->established ? (c->v12 ? post12_message(c, p[0]) : post_message(c, p[0], body, &keys_changed))
                       : c->v12 ? (c->is_server ? server12_message(c, p[0], body, whole) : client12_message(c, p[0], body, whole))
                       : c->is_server ? server_message(c, p[0], body, whole, &keys_changed)
                                      : client_message(c, p[0], body, whole, &keys_changed);
        if (e != PROVEN_OK) return e;
        at += 4 + mlen;
        if (keys_changed && at != total) return tls_unexpected(c);      /* bytes under the old keys after a key change */
        if (c->established != was_established) {
            /* The handshake buffer went away with the handshake; nothing was left in it. */
            return PROVEN_OK;
        }
    }
    /* Keep what is left of an incomplete message. */
    proven_size_t rest = acc->len - at;
    for (proven_size_t i = 0; i < rest; ++i) acc->ptr[i] = acc->ptr[at + i];
    acc->len = rest;
    if (rest == 0 && c->established) buf_free(c, &c->post);
    return PROVEN_OK;
}

/* A record of a TLS 1.2 connection. Three things differ from 1.3: the type travels in the
 * clear (and is authenticated); ChangeCipherSpec is where the read keys change, and is
 * accepted in exactly one place; and alerts have a level that means something. */
static proven_err_t record_process12(proven_tls_conn_t *c) {
    proven_byte_t type = c->in.ptr[0];
    proven_byte_t *body = c->in.ptr + PROVEN_TLS_RECORD_HEADER;
    proven_size_t len = c->in.len - PROVEN_TLS_RECORD_HEADER;
    proven_mem_mut_t content = { .ptr = body, .size = len };
    if (type == PROVEN_TLS_CT_CCS) {
        proven_tls_handshake_t *h = c->hs;
        /* Only where the handshake is waiting for it, only once, and never with part of a
         * handshake message still unread before it: a ChangeCipherSpec that arrives early
         * would switch on keys made from nothing the peer has proved. */
        bool due = h && (c->state == ST12_C_WAIT_CCS || c->state == ST12_S_WAIT_CCS) && h->reassembly.len == 0 && !c->live->read.active;
        if (!due || len != 1 || body[0] != 1) return tls_unexpected(c);
        c->live->read = h->next_read;
        tls_wipe(&h->next_read, sizeof h->next_read);
        c->state = c->state == ST12_C_WAIT_CCS ? ST12_C_WAIT_FINISHED : ST12_S_WAIT_FINISHED;
        return PROVEN_OK;
    }
    if (c->live->read.active) {
        /* One answer for every way a record can fail to open - a CBC record above all. */
        bool opened = c->suite12->mac ? proven_tls_cbc_open(&c->live->read, &c->cbc[c->is_server ? 0 : 1], c->in.ptr, len, &content)
                                      : proven_tls12_open(&c->live->read, c->in.ptr, len, &content);
        if (!opened) return tls_fail(c, AL_BAD_RECORD_MAC, PROVEN_ERR_PROTOCOL);
    } else {
        if (type == PROVEN_TLS_CT_APPLICATION) return tls_unexpected(c);
        if (len > PROVEN_TLS_MAX_PLAINTEXT) return tls_fail(c, AL_RECORD_OVERFLOW, PROVEN_ERR_PROTOCOL);
    }
    switch (type) {
        case PROVEN_TLS_CT_HANDSHAKE:
            /* After the ChangeCipherSpec the next thing is Finished, under the new keys. */
            if ((c->state == ST12_C_WAIT_FINISHED || c->state == ST12_S_WAIT_FINISHED) && !c->live->read.active) return tls_unexpected(c);
            /* Once established, a handshake record can only ask to renegotiate, and those
             * requests are what post12_message counts. */
            if (!c->established) c->ignored = 0;
            return handshake_bytes(c, content.ptr, content.size);
        case PROVEN_TLS_CT_ALERT:
            if (content.size != 2) return tls_decode(c);
            if (content.ptr[1] == AL_CLOSE_NOTIFY) { c->close_received = true; return PROVEN_OK; }
            /* A warning is a warning: user_canceled, no_renegotiation and the like are noted
             * and passed over, a bounded number of times. Anything fatal ends the connection. */
            if (content.ptr[0] == 1) return ++c->ignored > TLS_MAX_IGNORED ? tls_unexpected(c) : PROVEN_OK;
            c->alert_received = content.ptr[1];
            c->dead = true;
            c->error = PROVEN_ERR_PROTOCOL;
            return c->error;
        case PROVEN_TLS_CT_APPLICATION:
            if (!c->established) return tls_unexpected(c);
            if (content.size == 0) return ++c->ignored > TLS_MAX_IGNORED ? tls_unexpected(c) : PROVEN_OK;
            c->ignored = 0;
            c->plain_off = (proven_size_t)(content.ptr - c->in.ptr);
            c->plain_len = c->plain_off + content.size;
            return PROVEN_OK;
        default:
            return tls_unexpected(c);
    }
}

/* A TCP server passes over the early data it does not take (RFC 8446 section 4.2.10): after a
 * HelloRetryRequest, records of type application_data until the second ClientHello; otherwise
 * records that do not open under the client's handshake keys, until one does. Either way up to
 * TLS_EARLY_SKIP bytes, each record charged its header too, so that empty ones count. */
static bool early_skipped(proven_tls_conn_t *c, proven_size_t len, bool keyed) {
    proven_tls_handshake_t *h = c->hs;
    if (!c->is_server || !h || h->early_skip == 0) return false;
    if (keyed ? h->retried : (!h->retried || c->state != ST_S_WAIT_CH)) return false;
    const proven_size_t cost = PROVEN_TLS_RECORD_HEADER + len;
    if (cost > h->early_skip) return false;
    h->early_skip -= (proven_u32)cost;
    return true;
}

static proven_err_t record_process(proven_tls_conn_t *c) {
    if (c->v12) return record_process12(c);
    proven_byte_t type = c->in.ptr[0];
    proven_byte_t *body = c->in.ptr + PROVEN_TLS_RECORD_HEADER;
    proven_size_t len = c->in.len - PROVEN_TLS_RECORD_HEADER;
    proven_mem_mut_t content = { .ptr = body, .size = len };
    if (type == PROVEN_TLS_CT_CCS) {
        /* The compatibility ChangeCipherSpec: one byte, 1, in the clear, during the handshake
         * only - and never before the first hello of this side's peer has been seen. */
        if (c->established || len != 1 || body[0] != 1) return tls_unexpected(c);
        if (c->is_server && c->state == ST_S_WAIT_CH && !c->hs->retried) return tls_unexpected(c);
        /* One is expected. A peer that sends them without end is not making progress. */
        if (++c->ignored > TLS_MAX_IGNORED) return tls_unexpected(c);
        return PROVEN_OK;
    }
    if (c->live->read.active) {
        if (type != PROVEN_TLS_CT_APPLICATION) return tls_unexpected(c);
        if (!proven_tls13_open(&c->live->read, c->in.ptr, len, &type, &content)) {
            if (early_skipped(c, len, true)) return PROVEN_OK;
            return tls_fail(c, AL_BAD_RECORD_MAC, PROVEN_ERR_PROTOCOL);
        }
        if (c->hs) c->hs->early_skip = 0;                         /* the client's own records have begun */
    } else {
        if (type == PROVEN_TLS_CT_APPLICATION) return early_skipped(c, len, false) ? PROVEN_OK : tls_unexpected(c);
        if (len > PROVEN_TLS_MAX_PLAINTEXT) return tls_fail(c, AL_RECORD_OVERFLOW, PROVEN_ERR_PROTOCOL);
    }
    switch (type) {
        case PROVEN_TLS_CT_HANDSHAKE:
            c->ignored = 0;
            return handshake_bytes(c, content.ptr, content.size);
        case PROVEN_TLS_CT_ALERT:
            if (content.size != 2) return tls_decode(c);
            if (content.ptr[1] == AL_CLOSE_NOTIFY) { c->close_received = true; return PROVEN_OK; }
            if (content.ptr[1] == AL_USER_CANCELED) return ++c->ignored > TLS_MAX_IGNORED ? tls_unexpected(c) : PROVEN_OK;
            /* Every other alert is fatal in TLS 1.3 whatever its level says. Nothing is sent back. */
            c->alert_received = content.ptr[1];
            c->dead = true;
            c->error = PROVEN_ERR_PROTOCOL;
            return c->error;
        case PROVEN_TLS_CT_APPLICATION:
            if (!c->established) {
                /* Early data a server took (RFC-0027): read as application data until
                 * EndOfEarlyData, and never more than the ticket allowed - more is
                 * unexpected_message (RFC 8446 section 4.2.10). */
                if (!c->is_server || c->state != ST_S_WAIT_EOED) return tls_unexpected(c);
                if (content.size > c->hs->early_left) return tls_unexpected(c);
                c->hs->early_left -= (proven_u32)content.size;
                c->in_early = true;
            } else {
                c->in_early = false;
            }
            /* Empty records are allowed, and are how some peers hide traffic patterns; an
             * endless run of them is a peer keeping this side busy for nothing. */
            if (content.size == 0) return ++c->ignored > TLS_MAX_IGNORED ? tls_unexpected(c) : PROVEN_OK;
            c->ignored = 0;
            c->plain_off = (proven_size_t)(content.ptr - c->in.ptr);
            c->plain_len = c->plain_off + content.size;
            return PROVEN_OK;
        default:
            return tls_unexpected(c);
    }
}

/* ================= TLS 1.2 =================
 *
 * The part of RFC 5246 that RFC 9325 still stands behind: ECDHE, an AEAD, the extended master
 * secret, no renegotiation, no compression. What is shared with 1.3 above: the records'
 * framing, the handshake buffer, certificates and their verification, the signature check. */

static void tls12_establish(proven_tls_conn_t *c) {
    c->state = ST_ESTABLISHED;
    c->established = true;
    hs_free(c);
}

/* TLS 1.0 or 1.1: the same handshake as 1.2 with the older hashes and no AEAD. */
static bool tls_old(const proven_tls_conn_t *c) { return c->version != 0 && c->version < PROVEN_TLS_VERSION_1_2; }

/* The handshake so far as TLS 1.0 and 1.1 hash it: MD5 then SHA-1, 36 bytes. */
static void tls10_digest(proven_tls_conn_t *c, proven_byte_t out[36]) {
    proven_md5_t m = c->hs->tr_md5;
    proven_sha1_t s = c->hs->tr_sha1;
    proven_md5_final(&m, out);
    proven_sha1_final(&s, out + 16);
}

static void tls10_transcript_start(proven_tls_conn_t *c) {
    proven_md5_init(&c->hs->tr_md5);
    proven_sha1_init(&c->hs->tr_sha1);
    c->hs->tr_old = true;
}

/* The handshake hash as Finished and the master secret use it: under the suite's hash from
 * TLS 1.2 on, and both old hashes before. */
static proven_mem_view_t tls12_hash(proven_tls_conn_t *c, proven_byte_t out[PROVEN_HMAC_MAX_SIZE]) {
    if (tls_old(c)) { tls10_digest(c, out); return (proven_mem_view_t){ .ptr = out, .size = 36 }; }
    return (proven_mem_view_t){ .ptr = out, .size = proven_tls_transcript_hash(&c->hs->transcript, out) };
}

/* The PRF of whichever version was agreed. */
static void tls1x_prf(proven_tls_conn_t *c, proven_mem_view_t secret, const char *label, proven_mem_view_t seed, proven_byte_t *out, proven_size_t out_len) {
    if (tls_old(c)) proven_tls10_prf(secret, label, seed, out, out_len);
    else proven_tls12_prf(c->suite12->hash, secret, label, seed, out, out_len);
}

static void tls1x_finished(proven_tls_conn_t *c, bool server, proven_byte_t out[PROVEN_TLS12_VERIFY_SIZE]) {
    proven_byte_t th[PROVEN_HMAC_MAX_SIZE];
    tls1x_prf(c, (proven_mem_view_t){ .ptr = c->hs->master12, .size = PROVEN_TLS12_MASTER_SIZE }, server ? "server finished" : "client finished",
              tls12_hash(c, th), out, PROVEN_TLS12_VERIFY_SIZE);
}

static bool tls12_finished_send(proven_tls_conn_t *c) {
    proven_byte_t verify[PROVEN_TLS12_VERIFY_SIZE];
    tls1x_finished(c, c->is_server, verify);
    proven_wr_t w = hs_begin(c, HS_FINISHED);
    wr_bytes(&w, verify, sizeof verify);
    return hs_end(&w) && hs_send(c);
}

static proven_err_t tls12_finished_check(proven_tls_conn_t *c, proven_rd_t body) {
    proven_byte_t want[PROVEN_TLS12_VERIFY_SIZE];
    if (body.n != PROVEN_TLS12_VERIFY_SIZE) return tls_decode(c);
    tls1x_finished(c, !c->is_server, want);
    if (!proven_mem_equal_ct((proven_mem_view_t){ .ptr = want, .size = sizeof want }, (proven_mem_view_t){ .ptr = body.p, .size = body.n }))
        return tls_fail(c, AL_DECRYPT_ERROR, PROVEN_ERR_PROTOCOL);
    return PROVEN_OK;
}

/* Both directions' keys from the master secret, for whichever kind of suite was agreed. A CBC
 * suite keeps a little more per direction than the cipher, and that is allocated here: a
 * connection that never agrees on one never has it. */
static bool tls12_make_keys(proven_tls_conn_t *c, proven_tls_keys_t *client_write, proven_tls_keys_t *server_write) {
    proven_tls_handshake_t *h = c->hs;
    if (c->suite12->mac == 0) {
        proven_tls12_set_keys(client_write, server_write, c->suite12, h->master12, h->random, h->server_random);
        return true;
    }
    if (!c->cbc) {
        proven_result_mem_mut_t m = c->alloc.alloc_fn(c->alloc.ctx, 2 * sizeof *c->cbc, 16);
        if (m.err != PROVEN_OK) return false;
        c->cbc = (proven_tls_cbc_t *)(void *)m.value.ptr;
    }
    proven_tls_cbc_set_keys(client_write, server_write, c->cbc, c->suite12, c->version, h->etm, h->master12, h->random, h->server_random);
    return client_write->active && server_write->active;
}

/* From the shared key to both directions' keys: the master secret is bound to the handshake
 * so far (through ClientKeyExchange), which is what makes it "extended". This side's writing
 * key is returned; its reading key waits in the handshake for the peer's ChangeCipherSpec. */
static bool tls12_derive(proven_tls_conn_t *c, const proven_byte_t *premaster, proven_size_t premaster_len, proven_tls_keys_t *write) {
    proven_tls_handshake_t *h = c->hs;
    proven_byte_t th[PROVEN_HMAC_MAX_SIZE];
    proven_tls_keys_t client_write, server_write;
    const proven_mem_view_t pre = { .ptr = premaster, .size = premaster_len };
    if (h->ems) {
        tls1x_prf(c, pre, "extended master secret", tls12_hash(c, th), h->master12, PROVEN_TLS12_MASTER_SIZE);
    } else {
        /* The derivation before RFC 7627, for a peer that has no other and a configuration
         * that said it may be talked to: bound to the two randoms and to nothing else. */
        proven_byte_t randoms[64];
        for (int i = 0; i < 32; ++i) { randoms[i] = h->random[i]; randoms[32 + i] = h->server_random[i]; }
        tls1x_prf(c, pre, "master secret", (proven_mem_view_t){ .ptr = randoms, .size = sizeof randoms }, h->master12, PROVEN_TLS12_MASTER_SIZE);
    }
    if (!tls12_make_keys(c, &client_write, &server_write)) return false;
    *write = c->is_server ? server_write : client_write;
    h->next_read = c->is_server ? client_write : server_write;
    tls_wipe(&client_write, sizeof client_write); tls_wipe(&server_write, sizeof server_write);
    return true;
}

/* ChangeCipherSpec, in the clear, and from the next byte on this side writes under `write`. */
static bool tls12_send_ccs(proven_tls_conn_t *c, const proven_tls_keys_t *write) {
    static const proven_byte_t one = 1;
    if (!emit(c, PROVEN_TLS_CT_CCS, &one, 1, 0x03)) return false;
    c->live->write = *write;
    return true;
}

/* ---- Client ---- */

static proven_err_t client12_server_hello(proven_tls_conn_t *c, proven_u16 version, proven_rd_t random, proven_rd_t sid, proven_u16 suite_id, proven_rd_t exts, proven_mem_view_t whole) {
    proven_tls_handshake_t *h = c->hs;
    const proven_tls_config_t *cfg = c->config;
    if (h->retried) return tls_illegal(c);                        /* a HelloRetryRequest was TLS 1.3; this is not */
    const proven_tls12_suite_t *suite = proven_tls12_suite_find(suite_id);
    if (!suite12_allowed(cfg, suite)) return tls_illegal(c);     /* not one of those that were offered */
    /* Before 1.2 there is no AEAD and no SHA-256 MAC: only the suites with a SHA-1 MAC exist. */
    if (version < PROVEN_TLS_VERSION_1_2 && suite->mac != PROVEN_TLS_LH_SHA1) return tls_illegal(c);
    /* A server that knows 1.3 and was talked down to 1.2 says so in its random; this side
     * offered 1.3, so seeing that mark means somebody removed the offer on the way. */
    if (cfg->max_version > version &&
        (bytes_eq(random.p + 24, DOWNGRADE_SENTINEL, 8) || (bytes_eq(random.p + 24, DOWNGRADE_SENTINEL, 7) && random.p[31] == 0))) return tls_illegal(c);
    bool ems = false;
    unsigned seen = 0;
    while (exts.n > 0) {
        proven_u16 type = (proven_u16)rd_uint(&exts, 2);
        proven_rd_t data = rd_vec(&exts, 2);
        if (!exts.ok) return tls_decode(c);
        unsigned bit = type == EXT_EXTENDED_MASTER_SECRET ? 1u : type == EXT_RENEGOTIATION_INFO ? 2u : type == EXT_EC_POINT_FORMATS ? 4u :
                       type == EXT_ALPN ? 8u : type == EXT_SERVER_NAME ? 16u : type == EXT_SESSION_TICKET ? 32u :
                       type == EXT_ENCRYPT_THEN_MAC && (cfg->legacy & PROVEN_TLS_LEGACY_CBC) ? 64u : 0u;
        if (bit == 0) return tls_fail(c, AL_UNSUPPORTED_EXTENSION, PROVEN_ERR_PROTOCOL);       /* a server may only answer what was asked */
        if (seen & bit) return tls_illegal(c);
        seen |= bit;
        if (type == EXT_EXTENDED_MASTER_SECRET) {
            if (data.n != 0) return tls_decode(c);
            ems = true;
        } else if (type == EXT_RENEGOTIATION_INFO) {
            /* A first handshake: the value must be empty. */
            if (data.n != 1 || data.p[0] != 0) return tls_fail(c, AL_HANDSHAKE_FAILURE, PROVEN_ERR_PROTOCOL);
        } else if (type == EXT_ALPN) {
            if (cfg->alpn_count == 0) return tls_fail(c, AL_UNSUPPORTED_EXTENSION, PROVEN_ERR_PROTOCOL);
            proven_rd_t list = rd_vec(&data, 2);
            proven_rd_t name = rd_vec(&list, 1);
            if (!data.ok || data.n != 0 || !list.ok || list.n != 0 || name.n == 0) return tls_decode(c);
            c->alpn_index = -1;
            for (proven_size_t i = 0; i < cfg->alpn_count; ++i) {
                proven_u8str_view_t ours = proven_tls_config_alpn(cfg, i);
                if (ours.size == name.n && bytes_eq(ours.ptr, name.p, name.n)) c->alpn_index = (int)i;
            }
            if (c->alpn_index < 0) return tls_illegal(c);
        } else if (type == EXT_SESSION_TICKET) {
            /* "A NewSessionTicket will follow." Only an answer to the extension this side sent. */
            if (cfg->no_resumption) return tls_fail(c, AL_UNSUPPORTED_EXTENSION, PROVEN_ERR_PROTOCOL);
            if (data.n != 0) return tls_decode(c);
            h->ticket_expected = true;
        } else if (type == EXT_SERVER_NAME) {
            if (data.n != 0) return tls_decode(c);
        } else if (type == EXT_ENCRYPT_THEN_MAC) {
            /* Only for a suite that has a MAC to move (RFC 7366 section 3). */
            if (data.n != 0) return tls_decode(c);
            if (suite->mac == 0) return tls_illegal(c);
            h->etm = true;
        }
    }
    /* Without the extended master secret a session's keys are not bound to the handshake that
     * made them (the triple-handshake attack). Such a server is not talked to - unless the
     * configuration says it may be, and then nothing that attack needs is done on this
     * connection: no session is kept from it, and no certificate of this side's is shown. */
    if (!ems && !(cfg->legacy & PROVEN_TLS_LEGACY_NO_EXTENDED_MASTER_SECRET)) return tls_fail(c, AL_HANDSHAKE_FAILURE, PROVEN_ERR_PROTOCOL);
    h->ems = ems;
    c->v12 = true;
    c->suite12 = suite;
    c->version = version;
    if (version < PROVEN_TLS_VERSION_1_2) {
        tls10_transcript_start(c);
        proven_md5_update(&h->tr_md5, (proven_mem_view_t){ .ptr = h->first_hello.ptr, .size = h->first_hello.len });
        proven_sha1_update(&h->tr_sha1, (proven_mem_view_t){ .ptr = h->first_hello.ptr, .size = h->first_hello.len });
    }
    for (int i = 0; i < 32; ++i) h->server_random[i] = random.p[i];
    /* The handshake is hashed under both hashes from here: Finished uses the suite's, and a
     * CertificateVerify uses the signature's, which may be the other. */
    proven_tls_transcript_init(&h->transcript, suite->hash);
    proven_tls_transcript_init(&h->transcript_alt, suite->hash == PROVEN_HMAC_SHA256 ? PROVEN_HMAC_SHA384 : PROVEN_HMAC_SHA256);
    proven_tls_transcript_update(&h->transcript, (proven_mem_view_t){ .ptr = h->first_hello.ptr, .size = h->first_hello.len });
    proven_tls_transcript_update(&h->transcript_alt, (proven_mem_view_t){ .ptr = h->first_hello.ptr, .size = h->first_hello.len });
    h->transcript_ready = true;
    buf_free(c, &h->first_hello);
    tr_add(c, whole);
    /* A server that accepts the ticket says so by repeating the session id this side sent. */
    if (h->ticket_offered12 && sid.n > 0 && sid.n == h->session_id_len && bytes_eq(sid.p, h->session_id, sid.n)) {
        proven_tls_session_data_t *sess = (proven_tls_session_data_t *)(void *)c->session->opaque;
        if (sess->suite != suite_id) return tls_illegal(c);      /* a session is resumed under the suite it was made with */
        /* Sessions are made with the extended master secret, and resumed in the version they were made in. */
        if (version != sess->version || !ems) return tls_fail(c, AL_HANDSHAKE_FAILURE, PROVEN_ERR_PROTOCOL);
        for (int i = 0; i < PROVEN_TLS12_MASTER_SIZE; ++i) h->master12[i] = sess->psk[i];
        c->resumed = true;
        h->resuming12 = true;
        /* The server's identity is the one verified when the session was made; a pin holds for it too. */
        c->has_peer_key = sess->has_peer_key == 1;
        for (int i = 0; i < 32; ++i) c->peer_key_hash[i] = sess->peer_key_hash[i];
        if (cfg->pin_count > 0) {
            bool pinned = false;
            for (proven_size_t i = 0; c->has_peer_key && i < cfg->pin_count; ++i) pinned = pinned || bytes_eq(cfg->pins[i], c->peer_key_hash, 32);
            if (!pinned) { c->peer_fault = PROVEN_CERT_FAULT_PIN_MISMATCH; return tls_fail(c, AL_BAD_CERTIFICATE, PROVEN_ERR_UNTRUSTED); }
        }
        proven_tls_keys_t client_write, server_write;
        if (!tls12_make_keys(c, &client_write, &server_write)) return tls_nomem(c);
        h->next_read = server_write;
        h->next_write = client_write;
        tls_wipe(&client_write, sizeof client_write); tls_wipe(&server_write, sizeof server_write);
        c->state = ST12_C_WAIT_CCS;                               /* then the server's Finished comes first */
        return PROVEN_OK;
    }
    c->state = ST12_C_WAIT_CERT;
    return PROVEN_OK;
}

/* The handshake so far, hashed with `hash` - one of the two that are kept running. */
static bool tls12_handshake_digest(proven_tls_conn_t *c, proven_hmac_hash_t hash, proven_byte_t out[PROVEN_HMAC_MAX_SIZE]) {
    proven_tls_handshake_t *h = c->hs;
    const proven_tls_transcript_t *t = h->transcript.hash == hash ? &h->transcript : h->transcript_alt.hash == hash ? &h->transcript_alt : NULL;
    if (!t) return false;
    (void)proven_tls_transcript_hash(t, out);
    return true;
}

/* Keep the session for another connection: the master secret, under the ticket the server gave. */
static void client12_store_session(proven_tls_conn_t *c) {
    proven_tls_handshake_t *h = c->hs;
    if (!h->ems) return;                                          /* see client12_server_hello */
    if (!c->session || c->config->no_resumption || h->ticket12.len == 0 || h->ticket12.len > PROVEN_TLS_SESSION_TICKET_MAX || h->ticket12_lifetime == 0) return;
    proven_tls_session_data_t *s = (proven_tls_session_data_t *)(void *)c->session->opaque;
    tls_wipe(s, sizeof *s);
    s->suite = c->suite12->id;
    s->version = c->version;
    s->psk_len = PROVEN_TLS12_MASTER_SIZE;
    for (int i = 0; i < PROVEN_TLS12_MASTER_SIZE; ++i) s->psk[i] = h->master12[i];
    s->received_at = proven_tls_config_now(c->config);
    s->lifetime_s = h->ticket12_lifetime > 604800 ? 604800 : h->ticket12_lifetime;
    s->name_len = (proven_u16)c->server_name_len;
    for (proven_size_t i = 0; i < c->server_name_len; ++i) s->name[i] = c->server_name[i];
    s->has_peer_key = c->has_peer_key ? 1 : 0;
    for (int i = 0; i < 32; ++i) s->peer_key_hash[i] = c->peer_key_hash[i];
    s->ticket_len = (proven_u16)h->ticket12.len;
    for (proven_size_t i = 0; i < h->ticket12.len; ++i) s->ticket[i] = h->ticket12.ptr[i];
    s->magic = PROVEN_TLS_SESSION_MAGIC;
}

/* ServerKeyExchange: the server's ephemeral public key, signed together with both randoms. */
/* The signature on a ServerKeyExchange, in the form of the agreed version. */
static int ske_check(bool old, proven_u16 scheme, const proven_cert_t *leaf, proven_mem_view_t msg, proven_mem_view_t sig) {
    if (old) {
        proven_byte_t digest[36];
        sig_digest_old(msg, digest);
        return sig_check_old(leaf, digest, sig);
    }
    return sig_check(scheme, leaf, msg, sig, true);
}

/* A DHE ServerKeyExchange. The group is taken only if it is one of the six known here; this
 * side's value and the shared secret are computed at once and kept for the flight. */
static proven_err_t client12_key_exchange_dhe(proven_tls_conn_t *c, proven_rd_t body) {
    proven_tls_handshake_t *h = c->hs;
    const proven_byte_t *params = body.p;
    proven_rd_t p = rd_vec(&body, 2);
    proven_rd_t g = rd_vec(&body, 2);
    proven_rd_t theirs = rd_vec(&body, 2);
    const proven_size_t params_len = (proven_size_t)(body.p - params);
    const bool old = tls_old(c);
    proven_u16 scheme = old ? 0 : (proven_u16)rd_uint(&body, 2);
    proven_rd_t sig = rd_vec(&body, 2);
    if (!body.ok || body.n != 0 || sig.n == 0 || p.n == 0 || g.n == 0 || theirs.n == 0) return tls_decode(c);
    const proven_mem_view_t prime = { .ptr = p.p, .size = p.n };
    /* Too small, not a prime, or simply not known: all the same answer. */
    if (!proven_tls_dh_known(prime, (proven_mem_view_t){ .ptr = g.p, .size = g.n })) return tls_fail(c, AL_INSUFFICIENT_SECURITY, PROVEN_ERR_PROTOCOL);
    proven_cert_t leaf;
    if (h->peer_leaf.len == 0 || proven_cert_parse((proven_mem_view_t){ .ptr = h->peer_leaf.ptr, .size = h->peer_leaf.len }, &leaf) != PROVEN_OK) return tls_unexpected(c);
    if (params_len > 3 * (2 + PROVEN_TLS_DH_MAX + 1)) return tls_decode(c);
    proven_byte_t signed_part[64 + 3 * (2 + PROVEN_TLS_DH_MAX + 1)];
    proven_size_t n = 0;
    for (int i = 0; i < 32; ++i) signed_part[n++] = h->random[i];
    for (int i = 0; i < 32; ++i) signed_part[n++] = h->server_random[i];
    for (proven_size_t i = 0; i < params_len; ++i) signed_part[n++] = params[i];
    int ok = ske_check(old, scheme, &leaf, (proven_mem_view_t){ .ptr = signed_part, .size = n }, (proven_mem_view_t){ .ptr = sig.p, .size = sig.n });
    if (ok < 0) return tls_illegal(c);
    if (ok == 0) return tls_fail(c, AL_DECRYPT_ERROR, PROVEN_ERR_PROTOCOL);
    proven_byte_t mine[PROVEN_TLS_DH_MAX], shared[PROVEN_TLS_DH_MAX];
    proven_size_t plen = p.n, shared_len = 0;
    for (proven_size_t i = 0; i < p.n && p.p[i] == 0; ++i) plen--;
    const proven_size_t xl = proven_tls_dh_exponent_len(plen);
    proven_tls_config_random(c->config, h->dh_priv, xl);
    h->dh_priv[0] |= 0x80;
    if (!proven_tls_dh_public(prime, h->dh_priv, xl, mine)) return tls_fail(c, AL_INTERNAL_ERROR, PROVEN_ERR_PROTOCOL);
    if (!proven_tls_dh_shared(prime, h->dh_priv, xl, (proven_mem_view_t){ .ptr = theirs.p, .size = theirs.n }, shared, &shared_len)) return tls_illegal(c);
    h->dh.len = 0;
    bool kept = buf_append(c, &h->dh, mine, plen) && buf_append(c, &h->dh, shared, shared_len);
    tls_wipe(shared, sizeof shared);
    tls_wipe(h->dh_priv, sizeof h->dh_priv);
    if (!kept) return tls_nomem(c);
    h->dh_pub_len = plen;
    return PROVEN_OK;
}

static proven_err_t client12_key_exchange(proven_tls_conn_t *c, proven_rd_t body) {
    proven_tls_handshake_t *h = c->hs;
    if (c->suite12->kx == PROVEN_TLS_KX_DHE) return client12_key_exchange_dhe(c, body);
    const proven_byte_t *params = body.p;
    proven_u32 curve_type = rd_uint(&body, 1);
    proven_u16 group = (proven_u16)rd_uint(&body, 2);
    proven_rd_t point = rd_vec(&body, 1);
    proven_size_t params_len = (proven_size_t)(body.p - params);
    const bool old = tls_old(c);
    proven_u16 scheme = old ? 0 : (proven_u16)rd_uint(&body, 2);
    proven_rd_t sig = rd_vec(&body, 2);
    if (!body.ok || body.n != 0 || sig.n == 0) return tls_decode(c);
    if (curve_type != 3 || (group != GROUP_X25519 && group != GROUP_P256)) return tls_illegal(c);     /* a named curve this side offered */
    if (point.n != (group == GROUP_X25519 ? 32u : 65u)) return tls_illegal(c);
    proven_cert_t leaf;
    if (h->peer_leaf.len == 0 || proven_cert_parse((proven_mem_view_t){ .ptr = h->peer_leaf.ptr, .size = h->peer_leaf.len }, &leaf) != PROVEN_OK) return tls_unexpected(c);
    proven_byte_t signed_part[64 + 4 + 65];
    proven_size_t n = 0;
    for (int i = 0; i < 32; ++i) signed_part[n++] = h->random[i];
    for (int i = 0; i < 32; ++i) signed_part[n++] = h->server_random[i];
    for (proven_size_t i = 0; i < params_len; ++i) signed_part[n++] = params[i];
    int ok = ske_check(old, scheme, &leaf, (proven_mem_view_t){ .ptr = signed_part, .size = n }, (proven_mem_view_t){ .ptr = sig.p, .size = sig.n });
    if (ok < 0) return tls_illegal(c);
    if (ok == 0) return tls_fail(c, AL_DECRYPT_ERROR, PROVEN_ERR_PROTOCOL);
    h->ske_group = group;
    /* The point is kept in the cookie's space: a cookie is TLS 1.3's and unused here. */
    for (proven_size_t i = 0; i < point.n; ++i) h->cookie[i] = point.p[i];
    h->cookie_len = point.n;
    return PROVEN_OK;
}

/* ServerHelloDone has arrived: ClientKeyExchange, ChangeCipherSpec, Finished. */
static proven_err_t client12_flight(proven_tls_conn_t *c) {
    proven_tls_handshake_t *h = c->hs;
    proven_byte_t pub[65], shared[PROVEN_TLS_DH_MAX];
    proven_size_t pub_len = 0, shared_len = 32;
    proven_tls_keys_t write;
    const bool by_rsa = c->suite12->kx == PROVEN_TLS_KX_RSA, by_dhe = c->suite12->kx == PROVEN_TLS_KX_DHE;
    if (by_dhe) {
        /* Both were made when the server's value arrived. */
        if (h->dh_pub_len == 0 || h->dh.len <= h->dh_pub_len) return tls_unexpected(c);
        shared_len = h->dh.len - h->dh_pub_len;
        for (proven_size_t i = 0; i < shared_len; ++i) shared[i] = h->dh.ptr[h->dh_pub_len + i];
    } else if (by_rsa) {
        /* The premaster is this side's own: the version first offered, then 46 random bytes.
         * It reaches the server encrypted under the key in its certificate - and so can be
         * read by whoever holds that key, now or ever. */
        shared_len = 48;
        proven_tls_config_random(c->config, shared, 48);
        shared[0] = (proven_byte_t)(h->hello_version >> 8); shared[1] = (proven_byte_t)h->hello_version;       /* as the hello said, whatever was agreed since */
    } else {
        if (h->ske_group == GROUP_P256) { make_p256_key(c, pub); pub_len = 65; }
        else { proven_crypto_x25519_public(pub, h->x25519_priv); pub_len = 32; }
        if (!key_agree(c, h->ske_group, (proven_mem_view_t){ .ptr = h->cookie, .size = h->cookie_len }, shared)) return tls_illegal(c);
    }
    /* A certificate was asked for: this side's, or an empty list when it has none the server
     * can use. peer_sig_schemes holds the scheme to sign with, or 0. */
    const bool prove = h->cert_requested && h->peer_sig_schemes != 0;
    if (h->cert_requested && (!cert_build(c, prove) || !hs_send(c))) return tls_nomem(c);
    proven_wr_t w = hs_begin(c, HS12_CLIENT_KEY_EXCHANGE);
    if (by_rsa) {
        proven_cert_t leaf;
        proven_byte_t pad[PROVEN_CRYPTO_RSA_MAX_BITS / 8], sealed[PROVEN_CRYPTO_RSA_MAX_BITS / 8];
        proven_size_t sealed_len = 0;
        if (h->peer_leaf.len == 0 || proven_cert_parse((proven_mem_view_t){ .ptr = h->peer_leaf.ptr, .size = h->peer_leaf.len }, &leaf) != PROVEN_OK ||
            leaf.key_kind != PROVEN_CERT_KEY_RSA) return tls_unexpected(c);
        proven_tls_config_random(c->config, pad, sizeof pad);
        proven_byte_t sent[48];
        for (int i = 0; i < 48; ++i) sent[i] = shared[i];
        bool ok = proven_crypto_rsa_encrypt_pkcs1(leaf.rsa_n, leaf.rsa_e, (proven_mem_view_t){ .ptr = sent, .size = 48 }, pad, sealed, &sealed_len);
        tls_wipe(sent, sizeof sent);
        tls_wipe(pad, sizeof pad);
        if (!ok) { tls_wipe(shared, sizeof shared); return tls_illegal(c); }
        wr_uint(&w, (proven_u32)sealed_len, 2); wr_bytes(&w, sealed, sealed_len);
    } else if (by_dhe) {
        if (g_test12 & 512u) for (proven_size_t i = 0; i < h->dh_pub_len; ++i) h->dh.ptr[i] = i + 1 == h->dh_pub_len;    /* a test: the value 1 */
        wr_uint(&w, (proven_u32)h->dh_pub_len, 2); wr_bytes(&w, h->dh.ptr, h->dh_pub_len);
    } else {
        wr_uint(&w, (proven_u32)pub_len, 1); wr_bytes(&w, pub, pub_len);
    }
    if (!hs_end(&w) || !hs_send(c)) return tls_nomem(c);
    const bool derived = tls12_derive(c, shared, shared_len, &write);
    tls_wipe(shared, sizeof shared);
    if (!derived) return tls_nomem(c);
    if (prove) {
        /* CertificateVerify: a signature over every handshake message so far. */
        proven_byte_t digest[PROVEN_HMAC_MAX_SIZE];
        w = hs_begin(c, HS_CERTIFICATE_VERIFY);
        if (tls_old(c)) {
            tls10_digest(c, digest);
            if (!tls_sign_old(c, digest, &w) || !hs_end(&w) || !hs_send(c)) return tls_nomem(c);
        } else if (!tls12_handshake_digest(c, PROVEN_HMAC_SHA256, digest) || !tls_sign_digest(c, h->peer_sig_schemes, digest, &w) || !hs_end(&w) || !hs_send(c)) return tls_nomem(c);
    }
    bool sent = tls12_send_ccs(c, &write) && tls12_finished_send(c);
    tls_wipe(&write, sizeof write);
    if (!sent) return tls_nomem(c);
    c->state = ST12_C_WAIT_CCS;
    return PROVEN_OK;
}

static proven_err_t client12_message(proven_tls_conn_t *c, proven_byte_t type, proven_rd_t body, proven_mem_view_t whole) {
    proven_err_t e;
    bool empty = false;
    switch (c->state) {
        case ST12_C_WAIT_CERT: {
            if (type != HS_CERTIFICATE) return tls_unexpected(c);
            e = cert_process(c, body, &empty);
            if (e != PROVEN_OK) return e;
            if (empty) return tls_decode(c);
            /* The suite said what kind of key signs the key exchange; the certificate must have it. */
            proven_cert_t leaf;
            if (proven_cert_parse((proven_mem_view_t){ .ptr = c->hs->peer_leaf.ptr, .size = c->hs->peer_leaf.len }, &leaf) != PROVEN_OK) return tls_decode(c);
            if (c->suite12->rsa_auth != (leaf.key_kind == PROVEN_CERT_KEY_RSA)) return tls_illegal(c);
            tr_add(c, whole);
            /* A key exchange by RSA has no ServerKeyExchange: the certificate's key is the exchange. */
            c->state = c->suite12->kx == PROVEN_TLS_KX_RSA ? ST12_C_WAIT_DONE : ST12_C_WAIT_SKE;
            return PROVEN_OK; }
        case ST12_C_WAIT_SKE:
            if (type != HS12_SERVER_KEY_EXCHANGE) return tls_unexpected(c);
            e = client12_key_exchange(c, body);
            if (e != PROVEN_OK) return e;
            tr_add(c, whole);
            c->state = ST12_C_WAIT_DONE;
            return PROVEN_OK;
        case ST12_C_WAIT_DONE:
            if (type == HS_CERTIFICATE_REQUEST && !c->hs->cert_requested) {
                /* certificate_types, the signature schemes the server accepts, and the names
                 * of authorities (not used: this side has one certificate or none). */
                const bool old = tls_old(c);
                proven_rd_t types = rd_vec(&body, 1);
                proven_rd_t schemes = old ? (proven_rd_t){ body.p, 0, true } : rd_vec(&body, 2);       /* TLS 1.0 and 1.1 name none */
                proven_rd_t names = rd_vec(&body, 2);
                if (!body.ok || body.n != 0 || types.n == 0 || (!old && (schemes.n == 0 || schemes.n % 2 != 0))) return tls_decode(c);
                (void)names;
                const proven_tls_config_t *cfg = c->config;
                proven_u16 scheme = 0;
                /* An Ed25519 key signs the handshake itself, not its hash, and the handshake is
                 * not kept: in TLS 1.2 such a certificate is not presented. */
                if (cfg->key_kind == PROVEN_TLS_KEY_P256 && list_has_u16(schemes, SIG_ECDSA_P256_SHA256)) scheme = SIG_ECDSA_P256_SHA256;
                else if (cfg->key_kind == PROVEN_TLS_KEY_RSA) scheme = list_has_u16(schemes, SIG_RSA_PSS_RSAE_SHA256) ? SIG_RSA_PSS_RSAE_SHA256 : list_has_u16(schemes, SIG_RSA_PKCS1_SHA256) ? SIG_RSA_PKCS1_SHA256 : 0;
                /* There the key's kind is the scheme: any non-zero value means "sign". */
                if (old) scheme = cfg->key_kind == PROVEN_TLS_KEY_P256 || cfg->key_kind == PROVEN_TLS_KEY_RSA ? 1 : 0;
                /* And without the extended master secret this side shows no certificate at all. */
                if (!c->hs->ems) scheme = 0;
                c->hs->cert_requested = true;
                c->hs->peer_sig_schemes = scheme;
                tr_add(c, whole);
                return PROVEN_OK;
            }
            if (type != HS12_SERVER_HELLO_DONE) return tls_unexpected(c);
            if (body.n != 0) return tls_decode(c);
            tr_add(c, whole);
            return client12_flight(c);
        case ST12_C_WAIT_CCS: {
            /* Between the Finished of one side and the ChangeCipherSpec of the other, one
             * message may come: the ticket the server said it would send. */
            proven_tls_handshake_t *h = c->hs;
            if (type != HS_NEW_SESSION_TICKET || !h->ticket_expected) return tls_unexpected(c);
            proven_u32 lifetime = rd_uint(&body, 4);
            proven_rd_t ticket = rd_vec(&body, 2);
            if (!body.ok || body.n != 0) return tls_decode(c);
            h->ticket_expected = false;
            h->ticket12_lifetime = lifetime;
            h->ticket12.len = 0;
            /* One that does not fit is simply not kept: resumption is optional. */
            if (ticket.n > 0 && ticket.n <= PROVEN_TLS_SESSION_TICKET_MAX && !buf_append(c, &h->ticket12, ticket.p, ticket.n)) return tls_nomem(c);
            tr_add(c, whole);
            return PROVEN_OK; }
        case ST12_C_WAIT_FINISHED: {
            proven_tls_handshake_t *h = c->hs;
            if (type != HS_FINISHED) return tls_unexpected(c);
            e = tls12_finished_check(c, body);
            if (e != PROVEN_OK) return e;
            if (h->resuming12) {
                /* An abbreviated handshake: the server spoke first, and this side answers. */
                tr_add(c, whole);
                if (!tls12_send_ccs(c, &h->next_write) || !tls12_finished_send(c)) return tls_nomem(c);
            }
            client12_store_session(c);
            tls12_establish(c);
            return PROVEN_OK; }
        default:
            return tls_unexpected(c);
    }
}

/* ---- Server ---- */

/* A signature made over the hash of the handshake so far: TLS 1.2's CertificateVerify. The
 * schemes are the ones this side's CertificateRequest listed. 1 yes, 0 no, -1 not allowed. */
static int sig_check_handshake(proven_tls_conn_t *c, proven_u16 scheme, const proven_cert_t *leaf, proven_mem_view_t s) {
    proven_byte_t digest[PROVEN_HMAC_MAX_SIZE];
    const bool sha384 = scheme == SIG_ECDSA_P384_SHA384 || scheme == SIG_RSA_PSS_RSAE_SHA384 || scheme == SIG_RSA_PKCS1_SHA384;
    const proven_hmac_hash_t hash = sha384 ? PROVEN_HMAC_SHA384 : PROVEN_HMAC_SHA256;
    if (!tls12_handshake_digest(c, hash, digest)) return -1;
    proven_mem_view_t d = { .ptr = digest, .size = proven_hmac_size(hash) };
    switch (scheme) {
        case SIG_ECDSA_P256_SHA256:
            return leaf->key_kind != PROVEN_CERT_KEY_EC_P256 ? -1 : proven_crypto_ecdsa_verify_der(PROVEN_CRYPTO_EC_P256, leaf->key, d, s) ? 1 : 0;
        case SIG_ECDSA_P384_SHA384:
            return leaf->key_kind != PROVEN_CERT_KEY_EC_P384 ? -1 : proven_crypto_ecdsa_verify_der(PROVEN_CRYPTO_EC_P384, leaf->key, d, s) ? 1 : 0;
        case SIG_RSA_PSS_RSAE_SHA256: case SIG_RSA_PSS_RSAE_SHA384:
            return leaf->key_kind != PROVEN_CERT_KEY_RSA ? -1 : proven_crypto_rsa_verify_pss(leaf->rsa_n, leaf->rsa_e, hash, d.size, d, s) ? 1 : 0;
        case SIG_RSA_PKCS1_SHA256: case SIG_RSA_PKCS1_SHA384:
            return leaf->key_kind != PROVEN_CERT_KEY_RSA ? -1 : proven_crypto_rsa_verify_pkcs1(leaf->rsa_n, leaf->rsa_e, hash, d, s) ? 1 : 0;
        default:
            return -1;
    }
}

static proven_err_t server12_client_hello(proven_tls_conn_t *c, proven_u16 version, proven_rd_t random, proven_rd_t sid, proven_rd_t suites, proven_rd_t exts, proven_rd_t groups, proven_rd_t sig_algs, proven_rd_t alpn, proven_mem_view_t whole) {
    proven_tls_handshake_t *h = c->hs;
    const proven_tls_config_t *cfg = c->config;
    const bool old = version < PROVEN_TLS_VERSION_1_2;
    /* A client that fell back to an older version after failing with something newer says so;
     * if this side could have done better, the fallback was forced on it (RFC 7507). */
    if (list_has_u16(suites, 0x5600) && cfg->max_version > version) return tls_fail(c, 86, PROVEN_ERR_PROTOCOL);
    bool ems = false, points = false, wants_ticket = false, etm_offered = false;
    proven_rd_t offered_ticket = { 0 };
    {
        proven_rd_t scan = exts;
        while (scan.n > 0) {
            proven_u16 type = (proven_u16)rd_uint(&scan, 2);
            proven_rd_t data = rd_vec(&scan, 2);
            if (!scan.ok) return tls_decode(c);
            if (type == EXT_EXTENDED_MASTER_SECRET) {
                if (data.n != 0) return tls_decode(c);
                ems = true;
            } else if (type == EXT_RENEGOTIATION_INFO) {
                /* A first handshake carries an empty value; anything else is an attempt to continue one. */
                if (data.n != 1 || data.p[0] != 0) return tls_fail(c, AL_HANDSHAKE_FAILURE, PROVEN_ERR_PROTOCOL);
            } else if (type == EXT_EC_POINT_FORMATS) {
                points = true;
            } else if (type == EXT_ENCRYPT_THEN_MAC) {
                if (data.n != 0) return tls_decode(c);
                etm_offered = true;
            } else if (type == EXT_SESSION_TICKET) {
                wants_ticket = true;
                offered_ticket = data;
            }
        }
    }
    if (!ems && !(cfg->legacy & PROVEN_TLS_LEGACY_NO_EXTENDED_MASTER_SECRET)) return tls_fail(c, AL_HANDSHAKE_FAILURE, PROVEN_ERR_PROTOCOL);       /* see client12_server_hello */
    /* On such a connection a client certificate proves less than it seems to: one that this
     * configuration requires cannot be had, so the connection cannot either. */
    if (!ems && cfg->client_auth == PROVEN_TLS_CLIENT_AUTH_REQUIRE) return tls_fail(c, AL_HANDSHAKE_FAILURE, PROVEN_ERR_PROTOCOL);
    h->ems = ems;
    /* TLS 1.0 and 1.1 have no way to sign with an Ed25519 key (RFC 8422). */
    if (old && cfg->key_kind == PROVEN_TLS_KEY_ED25519) return tls_fail(c, AL_HANDSHAKE_FAILURE, PROVEN_ERR_PROTOCOL);

    /* The suite: for this side's kind of key, in this side's order. */
    const bool rsa = cfg->key_kind == PROVEN_TLS_KEY_RSA;
    proven_u16 order13[3];
    suite_order(order13);
    const proven_tls12_suite_t *suite = NULL;
    for (int i = 0; i < 3 && !suite && !old; ++i) if (list_has_u16(suites, suite12_for(order13[i], rsa))) suite = proven_tls12_suite_find(suite12_for(order13[i], rsa));
    /* Nothing of the six in common: then the first legacy suite, in the table's order, that
     * this configuration allows, the client offers and this key can serve - never one whose key
     * exchange is by RSA, which is a client's only (proven_tls_server_create). */
    const proven_tls12_suite_t *s12;
    for (proven_size_t i = 0; !suite && (s12 = proven_tls12_suite_at(i)) != NULL; ++i) {
        if (s12->legacy && s12->kx != PROVEN_TLS_KX_RSA && suite12_allowed(cfg, s12) && s12->rsa_auth == rsa && (!old || s12->mac == PROVEN_TLS_LH_SHA1) && list_has_u16(suites, s12->id)) suite = s12;
    }
    if (!suite) return tls_fail(c, AL_HANDSHAKE_FAILURE, PROVEN_ERR_PROTOCOL);

    /* The group, and the scheme the key exchange is signed with. Both lists are required:
     * without signature_algorithms RFC 5246 would mean SHA-1, which is not signed with here. */
    proven_u16 group = 0, scheme = 0;
    /* From TLS 1.2 the client must say which signatures it takes; before, the key's kind says it. */
    if (!old && (!sig_algs.ok || sig_algs.n % 2 != 0)) return tls_fail(c, AL_HANDSHAKE_FAILURE, PROVEN_ERR_PROTOCOL);
    if (suite->kx == PROVEN_TLS_KX_ECDHE) {
        if (!groups.ok || groups.n % 2 != 0) return tls_fail(c, AL_HANDSHAKE_FAILURE, PROVEN_ERR_PROTOCOL);
        group = list_has_u16(groups, GROUP_X25519) && !g_test_server_refuses_x25519 ? GROUP_X25519 : list_has_u16(groups, GROUP_P256) ? GROUP_P256 : 0;
        if (group == 0) return tls_fail(c, AL_HANDSHAKE_FAILURE, PROVEN_ERR_PROTOCOL);
    }
    scheme = our_scheme(cfg);
    if (!old && !list_has_u16(sig_algs, scheme)) {
        /* An RSA key may fall back to PKCS #1 v1.5 for a 1.2 client that predates PSS. */
        scheme = rsa && list_has_u16(sig_algs, SIG_RSA_PKCS1_SHA256) ? SIG_RSA_PKCS1_SHA256 : 0;
        if (scheme == 0) return tls_fail(c, AL_HANDSHAKE_FAILURE, PROVEN_ERR_PROTOCOL);
    }

    c->alpn_index = -1;
    if (h->client_sent_alpn && cfg->alpn_count > 0) {
        for (proven_size_t i = 0; i < cfg->alpn_count && c->alpn_index < 0; ++i) {
            proven_u8str_view_t ours = proven_tls_config_alpn(cfg, i);
            proven_rd_t list = alpn;
            while (list.n > 0) {
                proven_rd_t name = rd_vec(&list, 1);
                if (!list.ok || name.n == 0) return tls_decode(c);
                if (name.n == ours.size && bytes_eq(name.p, ours.ptr, name.n)) { c->alpn_index = (int)i; break; }
            }
        }
        if (c->alpn_index < 0) return tls_fail(c, AL_NO_APPLICATION_PROTOCOL, PROVEN_ERR_PROTOCOL);
    }

    /* Resumption: a ticket of this server, for a 1.2 session under a suite the client still
     * offers. It is only used when the client sent a session id, because repeating that id
     * is how the client is told. A server that requires a client certificate resumes only a
     * session in which one was shown. */
    proven_tls_ticket_state_t ticket = { 0 };
    bool resume = false;
    if (!cfg->no_resumption && ems && offered_ticket.n > 0 && sid.n > 0 &&
        proven_tls_ticket_open(cfg, (proven_mem_view_t){ .ptr = offered_ticket.p, .size = offered_ticket.n }, &ticket)) {
        const proven_tls12_suite_t *made = proven_tls12_suite_find(ticket.suite);
        resume = suite12_allowed(cfg, made) && ticket.psk_len == PROVEN_TLS12_MASTER_SIZE && list_has_u16(suites, ticket.suite) &&
                 (ticket.version ? ticket.version : PROVEN_TLS_VERSION_1_2) == version &&
                 !(cfg->client_auth == PROVEN_TLS_CLIENT_AUTH_REQUIRE && !ticket.has_peer_key);
        if (resume) suite = made;
    }
    /* Tickets are for sessions made with the extended master secret, and no others. */
    h->issue_ticket12 = wants_ticket && !cfg->no_resumption && !resume && ems;

    c->v12 = true;
    c->suite12 = suite;
    c->version = version;
    if (old) tls10_transcript_start(c);
    h->etm = etm_offered && suite->mac != 0;
    c->peer_record_limit = PROVEN_TLS_MAX_PLAINTEXT;               /* record_size_limit is not negotiated in 1.2 */
    proven_tls_transcript_init(&h->transcript, suite->hash);
    proven_tls_transcript_init(&h->transcript_alt, suite->hash == PROVEN_HMAC_SHA256 ? PROVEN_HMAC_SHA384 : PROVEN_HMAC_SHA256);
    h->transcript_ready = true;
    tr_add(c, whole);
    for (int i = 0; i < 32; ++i) h->random[i] = random.p[i];        /* the client's */
    proven_tls_config_random(cfg, h->server_random, 32);
    /* This side could have spoken 1.3 and is agreeing to 1.2: say so where a client that
     * offered 1.3 will look, so that it can tell a real choice from interference. */
    if (cfg->max_version > version) {
        for (int i = 0; i < 8; ++i) h->server_random[24 + i] = DOWNGRADE_SENTINEL[i];
        if (old) h->server_random[31] = 0;                         /* the mark for "1.1 or older" ends in 00 */
    }

    proven_wr_t w = hs_begin(c, HS_SERVER_HELLO);
    wr_uint(&w, version, 2); wr_bytes(&w, h->server_random, 32);
    /* No session id of this side's own: sessions are resumed by ticket or not at all. The
     * client's is repeated when its ticket is accepted. */
    if (resume) { wr_uint(&w, (proven_u32)sid.n, 1); wr_bytes(&w, sid.p, sid.n); }
    else wr_uint(&w, 0, 1);
    wr_uint(&w, suite->id, 2); wr_uint(&w, 0, 1);
    proven_size_t e = wr_open(&w, 2);
    wr_uint(&w, EXT_RENEGOTIATION_INFO, 2); wr_uint(&w, 1, 2); wr_uint(&w, 0, 1);
    if (ems && !(g_test12 & 4u)) { wr_uint(&w, EXT_EXTENDED_MASTER_SECRET, 2); wr_uint(&w, 0, 2); }
    if (points) { wr_uint(&w, EXT_EC_POINT_FORMATS, 2); wr_uint(&w, 2, 2); wr_uint(&w, 1, 1); wr_uint(&w, 0, 1); }
    if (c->alpn_index >= 0) {
        proven_u8str_view_t name = proven_tls_config_alpn(cfg, (proven_size_t)c->alpn_index);
        wr_uint(&w, EXT_ALPN, 2); wr_uint(&w, (proven_u32)name.size + 3, 2); wr_uint(&w, (proven_u32)name.size + 1, 2);
        wr_uint(&w, (proven_u32)name.size, 1); wr_bytes(&w, name.ptr, name.size);
    }
    if (h->client_sent_sni) { wr_uint(&w, EXT_SERVER_NAME, 2); wr_uint(&w, 0, 2); }
    if (h->issue_ticket12) { wr_uint(&w, EXT_SESSION_TICKET, 2); wr_uint(&w, 0, 2); }
    if (h->etm) { wr_uint(&w, EXT_ENCRYPT_THEN_MAC, 2); wr_uint(&w, 0, 2); }
    wr_close(&w, e, 2);
    if (!hs_end(&w) || !hs_send(c)) return tls_nomem(c);

    if (resume) {
        /* The abbreviated handshake: keys from the ticket's master secret and the two new
         * randoms, and this side's Finished at once. The client's follows. */
        proven_tls_keys_t client_write, server_write;
        for (int i = 0; i < PROVEN_TLS12_MASTER_SIZE; ++i) h->master12[i] = ticket.psk[i];
        if (!tls12_make_keys(c, &client_write, &server_write)) return tls_nomem(c);
        h->next_read = client_write;
        c->resumed = true;
        h->resuming12 = true;
        c->has_peer_key = ticket.has_peer_key;
        for (int i = 0; i < 32; ++i) c->peer_key_hash[i] = ticket.peer_key_hash[i];
        bool sent = tls12_send_ccs(c, &server_write) && tls12_finished_send(c);
        tls_wipe(&client_write, sizeof client_write); tls_wipe(&server_write, sizeof server_write); tls_wipe(&ticket, sizeof ticket);
        if (!sent) return tls_nomem(c);
        c->state = ST12_S_WAIT_CCS;
        return PROVEN_OK;
    }
    tls_wipe(&ticket, sizeof ticket);

    if (!cert_build(c, true) || !hs_send(c)) return tls_nomem(c);

    /* ServerKeyExchange: a fresh key on the agreed curve, signed with both randoms so that it
     * cannot be replayed into another handshake. */
    if (suite->kx == PROVEN_TLS_KX_ECDHE) {
    proven_byte_t pub[65], signed_part[64 + 4 + 65];
    proven_size_t pub_len, n = 0;
    if (group == GROUP_X25519) { proven_tls_config_random(cfg, h->x25519_priv, 32); proven_crypto_x25519_public(pub, h->x25519_priv); pub_len = 32; }
    else { make_p256_key(c, pub); pub_len = 65; }
    h->ske_group = group;
    for (int i = 0; i < 32; ++i) signed_part[n++] = h->random[i];
    for (int i = 0; i < 32; ++i) signed_part[n++] = h->server_random[i];
    const proven_size_t params_at = n;
    signed_part[n++] = 3; signed_part[n++] = (proven_byte_t)(group >> 8); signed_part[n++] = (proven_byte_t)group; signed_part[n++] = (proven_byte_t)pub_len;
    for (proven_size_t i = 0; i < pub_len; ++i) signed_part[n++] = pub[i];
    w = hs_begin(c, HS12_SERVER_KEY_EXCHANGE);
    wr_bytes(&w, signed_part + params_at, n - params_at);
    if (!ske_sign(c, scheme, (proven_mem_view_t){ .ptr = signed_part, .size = n }, &w) || !hs_end(&w) || !hs_send(c)) return tls_nomem(c);
    }
    if (suite->kx == PROVEN_TLS_KX_DHE) {
        /* The same message over a finite field: the prime, the generator and this side's
         * value, each with its length, signed with both randoms. The prime is ffdhe2048. */
        const proven_mem_view_t p = proven_tls_dh_server_group();
        const proven_size_t xl = proven_tls_dh_exponent_len(p.size);
        proven_byte_t signed_part[64 + 2 + 256 + 3 + 2 + 256];
        proven_size_t n = 0;
        proven_tls_config_random(cfg, h->dh_priv, xl);
        h->dh_priv[0] |= 0x80;                                       /* never a short exponent by chance */
        for (int i = 0; i < 32; ++i) signed_part[n++] = h->random[i];
        for (int i = 0; i < 32; ++i) signed_part[n++] = h->server_random[i];
        const proven_size_t params_at = n;
        signed_part[n++] = (proven_byte_t)(p.size >> 8); signed_part[n++] = (proven_byte_t)p.size;
        for (proven_size_t i = 0; i < p.size; ++i) signed_part[n++] = p.ptr[i];
        if (g_test12 & 128u) signed_part[n - 1] ^= 2;                 /* a test: a prime nobody knows */
        signed_part[n++] = 0; signed_part[n++] = 1; signed_part[n++] = 2;
        signed_part[n++] = (proven_byte_t)(p.size >> 8); signed_part[n++] = (proven_byte_t)p.size;
        if (!proven_tls_dh_public(p, h->dh_priv, xl, signed_part + n)) return tls_fail(c, AL_INTERNAL_ERROR, PROVEN_ERR_PROTOCOL);
        if (g_test12 & 256u) for (proven_size_t i = 0; i < p.size; ++i) signed_part[n + i] = i + 1 == p.size;     /* a test: the value 1 */
        n += p.size;
        w = hs_begin(c, HS12_SERVER_KEY_EXCHANGE);
        wr_bytes(&w, signed_part + params_at, n - params_at);
        if (!ske_sign(c, scheme, (proven_mem_view_t){ .ptr = signed_part, .size = n }, &w) || !hs_end(&w) || !hs_send(c)) return tls_nomem(c);
    }

    const bool ask_cert = cfg->client_auth != PROVEN_TLS_CLIENT_AUTH_NONE && (h->ems || (g_test12 & 4096u));       /* (the knob: ask all the same) */
    if (ask_cert) {
        /* The kinds of certificate, and the schemes a CertificateVerify may use: those whose
         * hash is one of the two the handshake is kept under. No authority names. */
        static const proven_u16 schemes[] = { SIG_ECDSA_P256_SHA256, SIG_ECDSA_P384_SHA384, SIG_RSA_PSS_RSAE_SHA256, SIG_RSA_PSS_RSAE_SHA384,
                                              SIG_RSA_PKCS1_SHA256, SIG_RSA_PKCS1_SHA384 };
        w = hs_begin(c, HS_CERTIFICATE_REQUEST);
        wr_uint(&w, 2, 1); wr_uint(&w, 64, 1); wr_uint(&w, 1, 1);         /* ecdsa_sign, rsa_sign */
        if (!old) {                                                       /* TLS 1.0 and 1.1 name no schemes */
            wr_uint(&w, (proven_u32)sizeof schemes, 2);
            for (proven_size_t i = 0; i < sizeof schemes / sizeof schemes[0]; ++i) wr_uint(&w, schemes[i], 2);
        }
        wr_uint(&w, 0, 2);
        if (!hs_end(&w) || !hs_send(c)) return tls_nomem(c);
    }
    w = hs_begin(c, HS12_SERVER_HELLO_DONE);
    if (!hs_end(&w) || !hs_send(c)) return tls_nomem(c);
    c->state = ask_cert ? ST12_S_WAIT_CERT : ST12_S_WAIT_CKE;
    return PROVEN_OK;
}

/* The ticket for this session, sent between the client's Finished and this side's
 * ChangeCipherSpec, and part of the handshake that this side's Finished covers. */
static bool server12_send_ticket(proven_tls_conn_t *c) {
    const proven_tls_config_t *cfg = c->config;
    proven_tls_ticket_state_t st = { 0 };
    proven_byte_t ticket[PROVEN_TLS_TICKET_MAX];
    st.suite = c->suite12->id;
    st.version = tls_old(c) ? c->version : 0;
    st.psk_len = PROVEN_TLS12_MASTER_SIZE;
    for (int i = 0; i < PROVEN_TLS12_MASTER_SIZE; ++i) st.psk[i] = c->hs->master12[i];
    st.issued_at = proven_tls_config_now(cfg);
    st.has_peer_key = c->has_peer_key;
    for (int i = 0; i < 32; ++i) st.peer_key_hash[i] = c->peer_key_hash[i];
    proven_size_t ticket_len = proven_tls_ticket_seal(cfg, &st, ticket);
    tls_wipe(&st, sizeof st);
    proven_wr_t w = hs_begin(c, HS_NEW_SESSION_TICKET);
    wr_uint(&w, cfg->ticket_lifetime_s, 4);
    wr_uint(&w, (proven_u32)ticket_len, 2); wr_bytes(&w, ticket, ticket_len);
    return hs_end(&w) && hs_send(c);
}

static proven_err_t server12_message(proven_tls_conn_t *c, proven_byte_t type, proven_rd_t body, proven_mem_view_t whole) {
    proven_tls_handshake_t *h = c->hs;
    proven_err_t e;
    switch (c->state) {
        case ST12_S_WAIT_CERT: {
            bool empty = false;
            if (type != HS_CERTIFICATE) return tls_unexpected(c);
            e = cert_process(c, body, &empty);
            if (e != PROVEN_OK) return e;
            if (empty) {
                /* TLS 1.2 has no alert that says "a certificate is required": handshake_failure. */
                if (c->config->client_auth == PROVEN_TLS_CLIENT_AUTH_REQUIRE) return tls_fail(c, AL_HANDSHAKE_FAILURE, PROVEN_ERR_UNTRUSTED);
            } else {
                h->client_cert_seen = true;
            }
            tr_add(c, whole);
            c->state = ST12_S_WAIT_CKE;
            return PROVEN_OK; }
        case ST12_S_WAIT_CV: {
            if (type != HS_CERTIFICATE_VERIFY) return tls_unexpected(c);
            const bool old = tls_old(c);
            proven_u16 scheme = old ? 0 : (proven_u16)rd_uint(&body, 2);
            proven_rd_t sig = rd_vec(&body, 2);
            proven_cert_t leaf;
            if (!body.ok || body.n != 0 || sig.n == 0) return tls_decode(c);
            if (h->peer_leaf.len == 0 || proven_cert_parse((proven_mem_view_t){ .ptr = h->peer_leaf.ptr, .size = h->peer_leaf.len }, &leaf) != PROVEN_OK) return tls_unexpected(c);
            int ok;
            if (old) {
                proven_byte_t digest[36];
                tls10_digest(c, digest);
                ok = sig_check_old(&leaf, digest, (proven_mem_view_t){ .ptr = sig.p, .size = sig.n });
            } else {
                ok = sig_check_handshake(c, scheme, &leaf, (proven_mem_view_t){ .ptr = sig.p, .size = sig.n });
            }
            if (ok < 0) return tls_illegal(c);
            if (ok == 0) return tls_fail(c, AL_DECRYPT_ERROR, PROVEN_ERR_PROTOCOL);
            tr_add(c, whole);
            c->state = ST12_S_WAIT_CCS;
            return PROVEN_OK; }
        case ST12_S_WAIT_CKE: {
            if (type != HS12_CLIENT_KEY_EXCHANGE) return tls_unexpected(c);
            proven_byte_t shared[PROVEN_TLS_DH_MAX];
            proven_size_t shared_len = 32;
            if (c->suite12->kx == PROVEN_TLS_KX_DHE) {
                const proven_mem_view_t p = proven_tls_dh_server_group();
                proven_rd_t theirs = rd_vec(&body, 2);
                if (!body.ok || body.n != 0) return tls_decode(c);
                if (!proven_tls_dh_shared(p, h->dh_priv, proven_tls_dh_exponent_len(p.size), (proven_mem_view_t){ .ptr = theirs.p, .size = theirs.n }, shared, &shared_len)) return tls_illegal(c);
            } else {
                proven_rd_t point = rd_vec(&body, 1);
                if (!body.ok || body.n != 0) return tls_decode(c);
                if (!key_agree(c, h->ske_group, (proven_mem_view_t){ .ptr = point.p, .size = point.n }, shared)) return tls_illegal(c);
            }
            /* The master secret covers the handshake through this message. */
            tr_add(c, whole);
            const bool derived = tls12_derive(c, shared, shared_len, &h->next_write);
            tls_wipe(shared, sizeof shared);
            if (!derived) return tls_nomem(c);
            /* A client that showed a certificate must now prove it holds the key. */
            c->state = h->client_cert_seen ? ST12_S_WAIT_CV : ST12_S_WAIT_CCS;
            return PROVEN_OK; }
        case ST12_S_WAIT_FINISHED: {
            if (type != HS_FINISHED) return tls_unexpected(c);
            e = tls12_finished_check(c, body);
            if (e != PROVEN_OK) return e;
            if (!h->resuming12) {
                tr_add(c, whole);                                  /* the server's Finished covers the client's */
                if (h->issue_ticket12 && !server12_send_ticket(c)) return tls_nomem(c);
                if (!tls12_send_ccs(c, &h->next_write) || !tls12_finished_send(c)) return tls_nomem(c);
            }
            tls12_establish(c);
            return PROVEN_OK; }
        default:
            return tls_unexpected(c);
    }
}

/* ---- After the handshake ---- */

/* The only handshake messages TLS 1.2 has after Finished ask for a new handshake. This side
 * does not renegotiate: it says so, as a warning, and carries on - a bounded number of times. */
static proven_err_t post12_message(proven_tls_conn_t *c, proven_byte_t type) {
    if (type != (c->is_server ? HS_CLIENT_HELLO : HS12_HELLO_REQUEST)) return tls_unexpected(c);
    if (++c->ignored > TLS_MAX_IGNORED) return tls_unexpected(c);
    static const proven_byte_t refusal[2] = { 1, AL_NO_RENEGOTIATION };
    return emit(c, PROVEN_TLS_CT_ALERT, refusal, 2, 0x03) ? PROVEN_OK : tls_nomem(c);
}

/* ================= The public face ================= */

static proven_err_t conn_new(const proven_tls_config_t *config, bool server, proven_tls_conn_t **out) {
    proven_allocator_t a = config->alloc;
    proven_result_mem_mut_t m = a.alloc_fn(a.ctx, sizeof(proven_tls_conn_t), 16);
    if (m.err != PROVEN_OK) return m.err;
    proven_tls_conn_t *c = (proven_tls_conn_t *)m.value.ptr;
    for (proven_size_t i = 0; i < sizeof *c; ++i) ((proven_byte_t *)c)[i] = 0;
    m = a.alloc_fn(a.ctx, sizeof(proven_tls_handshake_t), 16);
    if (m.err != PROVEN_OK) { a.free_fn(a.ctx, c); return m.err; }
    c->hs = (proven_tls_handshake_t *)m.value.ptr;
    for (proven_size_t i = 0; i < sizeof *c->hs; ++i) ((proven_byte_t *)c->hs)[i] = 0;
    m = a.alloc_fn(a.ctx, sizeof(proven_tls_live_keys_t), 16);
    if (m.err != PROVEN_OK) { a.free_fn(a.ctx, c->hs); a.free_fn(a.ctx, c); return m.err; }
    c->live = (proven_tls_live_keys_t *)m.value.ptr;
    for (proven_size_t i = 0; i < sizeof *c->live; ++i) ((proven_byte_t *)c->live)[i] = 0;
    c->config = config;
    c->alloc = a;
    c->is_server = server;
    c->alert_sent = -1; c->alert_received = -1; c->alpn_index = -1;
    c->peer_record_limit = PROVEN_TLS_MAX_PLAINTEXT;
    *out = c;
    return PROVEN_OK;
}

#ifndef PROVEN_NO_HTTP3
/* QUIC mode for a new connection: TLS 1.3 only, a protocol to name (`only`, when given, one of
 * the configuration's), and parameters that fit. */
static proven_err_t quic_attach(proven_tls_conn_t *c, proven_mem_view_t tp, proven_u8str_view_t only) {
    const proven_tls_config_t *cfg = c->config;
    if (cfg->max_version < PROVEN_TLS_VERSION_1_3 || cfg->alpn_count == 0 || tp.size > TLS_QUIC_TP_MAX || (tp.size > 0 && !tp.ptr)) return PROVEN_ERR_INVALID_ARG;
    int only_index = -1;
    for (proven_size_t i = 0; only.size > 0 && i < cfg->alpn_count; ++i) {
        const proven_u8str_view_t ours = proven_tls_config_alpn(cfg, i);
        if (ours.size == only.size && bytes_eq(ours.ptr, only.ptr, only.size)) only_index = (int)i;
    }
    if (only.size > 0 && only_index < 0) return PROVEN_ERR_INVALID_ARG;
    proven_result_mem_mut_t m = c->alloc.alloc_fn(c->alloc.ctx, sizeof(proven_tls_quic_t), 16);
    if (m.err != PROVEN_OK) return m.err;
    c->quic = (proven_tls_quic_t *)m.value.ptr;
    tls_wipe(c->quic, sizeof *c->quic);
    for (proven_size_t i = 0; i < tp.size; ++i) c->quic->tp_local[i] = tp.ptr[i];
    c->quic->tp_local_len = tp.size;
    c->quic->alpn_only = only_index;
    return PROVEN_OK;
}
#endif

static proven_err_t client_create(const proven_tls_config_t *config, proven_u8str_view_t server_name, proven_tls_session_t *session,
                                  proven_mem_view_t hello, const proven_byte_t *x25519_private, const proven_mem_view_t *quic_tp, proven_u8str_view_t quic_alpn,
                                  proven_u32 quic_version, bool early, proven_tls_conn_t **out) {
    if (!out || !config || (server_name.size > 0 && !server_name.ptr) || server_name.size > 253) return PROVEN_ERR_INVALID_ARG;
    if (server_name.size == 0 && config->verify != PROVEN_TLS_VERIFY_PIN_ONLY) return PROVEN_ERR_INVALID_ARG;
    if (config->verify == PROVEN_TLS_VERIFY_CHAIN && !config->anchors) return PROVEN_ERR_INVALID_ARG;
    proven_tls_conn_t *c = NULL;
    proven_err_t e = conn_new(config, false, &c);
    if (e != PROVEN_OK) return e;
    proven_tls_handshake_t *h = c->hs;
    c->session = session;
    if (server_name.size > 0) {
        proven_result_mem_mut_t m = c->alloc.alloc_fn(c->alloc.ctx, server_name.size, 1);
        if (m.err != PROVEN_OK) { proven_tls_conn_destroy(c); return m.err; }
        c->server_name = m.value.ptr;
    }
    for (proven_size_t i = 0; i < server_name.size; ++i) c->server_name[i] = server_name.ptr[i];
    c->server_name_len = server_name.size;
    if (c->server_name_len > 0 && c->server_name[c->server_name_len - 1] == '.') c->server_name_len--;
    proven_tls_config_random(config, h->random, 32);
    /* A session id of 32 random bytes makes the handshake look like a resumption of TLS 1.2
     * to boxes in the middle that would otherwise drop it. It means nothing else - and nothing
     * at all in QUIC, which forbids it (RFC 9001 section 8.4). */
    proven_tls_config_random(config, h->session_id, 32);
    h->session_id_len = 32;
#ifndef PROVEN_NO_HTTP3
    if (quic_tp) {
        e = quic_attach(c, *quic_tp, quic_alpn);
        if (e != PROVEN_OK) { proven_tls_conn_destroy(c); return e; }
        c->quic->version = c->quic->orig_version = quic_version;
        h->session_id_len = 0;
    }
#else
    (void)quic_tp; (void)quic_alpn; (void)quic_version;
#endif
    proven_tls_config_random(config, h->x25519_priv, 32);
    if (hello.size > 0) {
        if (hello.size > sizeof h->test_hello || hello.size < 39 + 32) { proven_tls_conn_destroy(c); return PROVEN_ERR_INVALID_ARG; }
        for (proven_size_t i = 0; i < hello.size; ++i) h->test_hello[i] = hello.ptr[i];
        h->test_hello_len = hello.size;
        for (int i = 0; i < 32; ++i) h->x25519_priv[i] = x25519_private[i];
    }
    h->want_early = early;
    c->state = ST_C_WAIT_SH;
    e = client_hello_send(c);
    if (e != PROVEN_OK) { proven_tls_conn_destroy(c); return e; }
    *out = c;
    return PROVEN_OK;
}

/* For tests: send a handshake message of `type` with no body under the current keys - the
 * shape of a HelloRequest, and close enough to a second ClientHello for a peer to refuse. */
bool proven_tls_test_send_handshake(proven_tls_conn_t *conn, proven_byte_t type) {
    const proven_byte_t msg[4] = { type, 0, 0, 0 };
    return conn && emit(conn, PROVEN_TLS_CT_HANDSHAKE, msg, 4, 0x03);
}

void proven_tls_test_peek_write(const proven_tls_conn_t *conn, proven_tls_keys_t *keys, proven_byte_t secret[48]) {
    if (conn->live) *keys = conn->live->write;
    else proven_tls_seed_give_(&conn->seed[1], keys);
    for (int i = 0; i < 48; ++i) secret[i] = conn->write_secret[i];
}

/* A client over TCP, offering early data or not. */
static proven_err_t client_create_with(const proven_tls_config_t *config, proven_u8str_view_t server_name,
                                       proven_tls_session_t *session, bool early, proven_tls_conn_t **out) {
    return client_create(config, server_name, session, (proven_mem_view_t){ .ptr = NULL, .size = 0 }, NULL, NULL, (proven_u8str_view_t){ 0 }, 0, early, out);
}

proven_err_t proven_tls_client_create(const proven_tls_config_t *config, proven_u8str_view_t server_name,
                                      proven_tls_session_t *session, proven_tls_conn_t **out) {
    return client_create_with(config, server_name, session, false, out);
}

proven_err_t proven_tls_client_create_early_(const proven_tls_config_t *config, proven_u8str_view_t server_name,
                                             proven_tls_session_t *session, proven_tls_conn_t **out) {
    return client_create_with(config, server_name, session, true, out);
}

proven_err_t proven_tls_client_create_replay(const proven_tls_config_t *config, proven_u8str_view_t server_name,
                                             proven_mem_view_t hello, const proven_byte_t x25519_private[32], proven_tls_conn_t **out) {
    if (!hello.ptr || !x25519_private) return PROVEN_ERR_INVALID_ARG;
    return client_create(config, server_name, NULL, hello, x25519_private, NULL, (proven_u8str_view_t){ 0 }, 0, false, out);
}

proven_err_t proven_tls_server_create(const proven_tls_config_t *config, proven_tls_conn_t **out) {
    if (!out || !config || config->chain_count == 0 || config->key_kind == PROVEN_TLS_KEY_NONE) return PROVEN_ERR_INVALID_ARG;
    /* A server that decrypts what a client chose is one mistake away from signing anything with
     * its key, in every protocol the key serves (Bleichenbacher; ROBOT). The bit is a client's. */
    if (config->legacy & PROVEN_TLS_LEGACY_RSA_KEY_EXCHANGE) return PROVEN_ERR_INVALID_ARG;
    proven_tls_conn_t *c = NULL;
    proven_err_t e = conn_new(config, true, &c);
    if (e != PROVEN_OK) return e;
    c->state = ST_S_WAIT_CH;
    *out = c;
    return PROVEN_OK;
}

#ifndef PROVEN_NO_HTTP3
/* ---- QUIC mode (RFC 9001 section 4): proven_internal_quic.h ---- */

proven_err_t proven_tls_quic_client_create_(const proven_tls_config_t *config, proven_u8str_view_t server_name, proven_tls_session_t *session,
                                            proven_mem_view_t transport_params, proven_tls_conn_t **out) {
    return proven_tls_quic_client_create_for_(config, server_name, session, transport_params, (proven_u8str_view_t){ 0 }, out);
}

proven_err_t proven_tls_quic_client_create_for_(const proven_tls_config_t *config, proven_u8str_view_t server_name, proven_tls_session_t *session,
                                                proven_mem_view_t transport_params, proven_u8str_view_t alpn, proven_tls_conn_t **out) {
    return proven_tls_quic_client_create_v_(config, server_name, session, transport_params, alpn, PROVEN_QUIC_VERSION_1, out);
}

proven_err_t proven_tls_quic_client_create_v_(const proven_tls_config_t *config, proven_u8str_view_t server_name, proven_tls_session_t *session,
                                              proven_mem_view_t transport_params, proven_u8str_view_t alpn, proven_u32 version, proven_tls_conn_t **out) {
    return client_create(config, server_name, session, (proven_mem_view_t){ .ptr = NULL, .size = 0 }, NULL, &transport_params, alpn, version, false, out);
}

proven_err_t proven_tls_quic_server_create_(const proven_tls_config_t *config, proven_mem_view_t transport_params, proven_tls_conn_t **out) {
    return proven_tls_quic_server_create_for_(config, transport_params, (proven_u8str_view_t){ 0 }, out);
}

proven_err_t proven_tls_quic_server_create_for_(const proven_tls_config_t *config, proven_mem_view_t transport_params, proven_u8str_view_t alpn, proven_tls_conn_t **out) {
    return proven_tls_quic_server_create_v_(config, transport_params, alpn, PROVEN_QUIC_VERSION_1, 0, out);
}

proven_err_t proven_tls_quic_server_create_v_(const proven_tls_config_t *config, proven_mem_view_t transport_params, proven_u8str_view_t alpn,
                                              proven_u32 version, proven_u32 upgrade_to, proven_tls_conn_t **out) {
    proven_tls_conn_t *c = NULL;
    proven_err_t e = proven_tls_server_create(config, &c);
    if (e != PROVEN_OK) return e;
    e = quic_attach(c, transport_params, alpn);
    if (e != PROVEN_OK) { proven_tls_conn_destroy(c); return e; }
    c->quic->version = c->quic->orig_version = version;
    c->quic->upgrade_to = upgrade_to;
    *out = c;
    return PROVEN_OK;
}

proven_u32 proven_tls_quic_version_(const proven_tls_conn_t *c) { return c && c->quic ? c->quic->version : 0; }
void proven_tls_quic_set_version_(proven_tls_conn_t *c, proven_u32 version) { if (c && c->quic) c->quic->version = version; }

proven_err_t proven_tls_quic_feed_(proven_tls_conn_t *c, proven_quic_level_t level, proven_mem_view_t in) {
    if (!c || !c->quic || (unsigned)level >= PROVEN_QUIC_LEVELS || (in.size > 0 && !in.ptr)) return PROVEN_ERR_INVALID_ARG;
    if (c->dead) return PROVEN_ERR_INVALID_STATE;
    if (in.size == 0) return PROVEN_OK;
    /* Handshake bytes are read at the level they were sent at, and only that one: bytes at
     * another level are a message out of place (RFC 9001 section 4.1.3). */
    if ((unsigned)level != c->quic->read_level) return tls_unexpected(c);
    return handshake_bytes(c, in.ptr, in.size);
}

proven_mem_view_t proven_tls_quic_pending_(const proven_tls_conn_t *c, proven_quic_level_t level) {
    if (!c || !c->quic || (unsigned)level >= PROVEN_QUIC_LEVELS) return (proven_mem_view_t){ .ptr = NULL, .size = 0 };
    const proven_tls_buf_t *b = &c->quic->out[level];
    const proven_size_t off = c->quic->out_off[level];
    if (b->len <= off) return (proven_mem_view_t){ .ptr = NULL, .size = 0 };
    return (proven_mem_view_t){ .ptr = b->ptr + off, .size = b->len - off };
}

void proven_tls_quic_sent_(proven_tls_conn_t *c, proven_quic_level_t level, proven_size_t n) {
    if (!c || !c->quic || (unsigned)level >= PROVEN_QUIC_LEVELS) return;
    proven_tls_buf_t *b = &c->quic->out[level];
    proven_size_t left = b->len - c->quic->out_off[level];
    c->quic->out_off[level] += n < left ? n : left;
    if (c->quic->out_off[level] == b->len) { buf_free(c, b); c->quic->out_off[level] = 0; }
}

proven_size_t proven_tls_quic_secret_(const proven_tls_conn_t *c, proven_quic_level_t level, bool write, proven_byte_t out[PROVEN_HMAC_MAX_SIZE]) {
    if (!c || !c->quic || !c->suite || level == PROVEN_QUIC_LEVEL_INITIAL || (unsigned)level >= PROVEN_QUIC_LEVELS || !c->quic->secret_ready[level][write ? 1 : 0]) return 0;
    for (proven_size_t i = 0; i < c->suite->hash_len; ++i) out[i] = c->quic->secret[level][write ? 1 : 0][i];
    return c->suite->hash_len;
}

void proven_tls_quic_secret_drop_(proven_tls_conn_t *c, proven_quic_level_t level) {
    if (!c || !c->quic || (unsigned)level >= PROVEN_QUIC_LEVELS) return;
    tls_wipe(c->quic->secret[level], sizeof c->quic->secret[level]);
    c->quic->secret_ready[level][0] = c->quic->secret_ready[level][1] = false;
}

const proven_tls_suite_t *proven_tls_quic_suite_(const proven_tls_conn_t *c) { return c && c->quic ? c->suite : NULL; }

const proven_quic_tp_t *proven_tls_quic_peer_params_(const proven_tls_conn_t *c) { return c && c->quic && c->quic->have_peer ? &c->quic->tp_peer : NULL; }

proven_quic_level_t proven_tls_quic_read_level_(const proven_tls_conn_t *c) { return c && c->quic ? (proven_quic_level_t)c->quic->read_level : PROVEN_QUIC_LEVEL_INITIAL; }
proven_quic_level_t proven_tls_quic_write_level_(const proven_tls_conn_t *c) { return c && c->quic ? (proven_quic_level_t)c->quic->write_level : PROVEN_QUIC_LEVEL_INITIAL; }

proven_u64 proven_tls_quic_error_(const proven_tls_conn_t *c) { return c && c->quic ? c->quic->error : 0; }

int proven_tls_quic_early_(const proven_tls_conn_t *c) { return c && c->quic ? c->quic->early : PROVEN_TLS_EARLY_NONE; }

proven_size_t proven_tls_quic_early_secret_(const proven_tls_conn_t *c, proven_byte_t out[PROVEN_HMAC_MAX_SIZE], const proven_tls_suite_t **suite) {
    if (!c || !c->quic || !c->quic->early_ready || !c->quic->early_suite) return 0;
    const proven_size_t n = c->quic->early_suite->hash_len;
    for (proven_size_t i = 0; i < n; ++i) out[i] = c->quic->early_secret[i];
    *suite = c->quic->early_suite;
    return n;
}

void proven_tls_quic_early_drop_(proven_tls_conn_t *c) {
    if (!c || !c->quic) return;
    tls_wipe(c->quic->early_secret, sizeof c->quic->early_secret);
    c->quic->early_ready = false;
}

bool proven_tls_quic_early_limits_(const proven_tls_conn_t *c, proven_u64 out[7]) {
    if (!c || !c->quic || c->is_server || c->quic->early != PROVEN_TLS_EARLY_OFFERED || !c->session) return false;
    const proven_tls_session_data_t *s = (const proven_tls_session_data_t *)(const void *)c->session->opaque;
    if (s->magic != PROVEN_TLS_SESSION_MAGIC || !s->has_limits) return false;
    for (int i = 0; i < 7; ++i) out[i] = s->limits[i];
    return true;
}
#endif

/* ---- A session's NEW_TOKEN token (RFC-0021 P-7): the QUIC layer's, kept beside the ticket ---- */

#ifndef PROVEN_NO_HTTP3
/* A record of this layout, or an empty one: the token's place is only trusted then. */
static bool session_token_place(const proven_tls_session_data_t *s) {
    return (s->magic == PROVEN_TLS_SESSION_MAGIC || s->magic == 0) && s->token_len <= PROVEN_TLS_SESSION_TOKEN_MAX;
}

proven_size_t proven_tls_session_token_take_(proven_tls_session_t *session, proven_byte_t *out, proven_size_t cap) {
    return proven_tls_session_token_take_v_(session, PROVEN_QUIC_VERSION_1, out, cap);
}

proven_size_t proven_tls_session_token_take_v_(proven_tls_session_t *session, proven_u32 version, proven_byte_t *out, proven_size_t cap) {
    if (!session) return 0;
    proven_tls_session_data_t *s = (proven_tls_session_data_t *)(void *)session->opaque;
    if (!session_token_place(s) || s->token_len == 0 || s->token_len > cap) return 0;
    if ((s->token_v2 != 0) != (version == PROVEN_QUIC_VERSION_2)) return 0;       /* another version's (RFC 9369 section 5) */
    const proven_size_t n = s->token_len;
    for (proven_size_t i = 0; i < n; ++i) out[i] = s->token[i];
    tls_wipe(s->token, sizeof s->token);
    s->token_len = 0;
    return n;
}

#endif

void proven_tls_session_early_off_(proven_tls_session_t *session) {
    if (!session) return;
    proven_tls_session_data_t *s = (proven_tls_session_data_t *)(void *)session->opaque;
    if (s->magic == PROVEN_TLS_SESSION_MAGIC) s->early = 0;
}

#ifndef PROVEN_NO_HTTP3
void proven_tls_session_token_keep_(proven_tls_session_t *session, proven_mem_view_t token) {
    proven_tls_session_token_keep_v_(session, PROVEN_QUIC_VERSION_1, token);
}

void proven_tls_session_token_keep_v_(proven_tls_session_t *session, proven_u32 version, proven_mem_view_t token) {
    if (!session || !token.ptr || token.size == 0 || token.size > PROVEN_TLS_SESSION_TOKEN_MAX) return;
    proven_tls_session_data_t *s = (proven_tls_session_data_t *)(void *)session->opaque;
    if (!session_token_place(s)) tls_wipe(s, sizeof *s);              /* an older layout: nothing of it is used */
    tls_wipe(s->token, sizeof s->token);
    for (proven_size_t i = 0; i < token.size; ++i) s->token[i] = token.ptr[i];
    s->token_len = (proven_u16)token.size;
    s->token_v2 = version == PROVEN_QUIC_VERSION_2 ? 1 : 0;
}

bool proven_tls_session_quic_v2_(const proven_tls_session_t *session) {
    if (!session) return false;
    const proven_tls_session_data_t *s = (const proven_tls_session_data_t *)(const void *)session->opaque;
    return s->magic == PROVEN_TLS_SESSION_MAGIC && s->ticket_len > 0 && s->ticket_v2;
}
#endif

/* Keep what remakes one direction's keys: for AES the key the schedule began from (the first
 * round keys are the key itself), for ChaCha20 the key it keeps where AES keeps its round keys. */
void proven_tls_seed_take_(proven_tls_key_seed_t *seed, const proven_tls_keys_t *k) {
    seed->active = k->active;
    seed->chacha = k->chacha;
    seed->key_len = (proven_u8)(k->chacha || k->aes.rounds == 14 ? 32 : 16);
    for (int i = 0; i < seed->key_len; ++i) seed->key[i] = k->aes.rk[i];
    for (int i = 0; i < 12; ++i) seed->iv[i] = k->iv[i];
    seed->seq = k->seq;
}

void proven_tls_seed_give_(const proven_tls_key_seed_t *seed, proven_tls_keys_t *k) {
    k->active = seed->active;
    k->chacha = seed->chacha;
    if (seed->chacha) for (int i = 0; i < 32; ++i) k->aes.rk[i] = seed->key[i];
    else if (seed->active) (void)proven_crypto_aes_gcm_init(&k->aes, seed->key, seed->key_len);
    for (int i = 0; i < 12; ++i) k->iv[i] = seed->iv[i];
    k->seq = seed->seq;
}

/* The keys, remade if the connection was compacted. False when there is no memory for them: the
 * connection is then dead with PROVEN_ERR_NOMEM - without an alert, which would need them. */
static bool keys_live(proven_tls_conn_t *c) {
    if (c->live) return true;
    proven_result_mem_mut_t m = c->alloc.alloc_fn(c->alloc.ctx, sizeof(proven_tls_live_keys_t), 16);
    if (m.err != PROVEN_OK) {
        if (!c->dead) { c->dead = true; c->error = PROVEN_ERR_NOMEM; }
        return false;
    }
    c->live = (proven_tls_live_keys_t *)m.value.ptr;
    for (proven_size_t i = 0; i < sizeof *c->live; ++i) ((proven_byte_t *)c->live)[i] = 0;
    proven_tls_seed_give_(&c->seed[0], &c->live->read);
    proven_tls_seed_give_(&c->seed[1], &c->live->write);
    tls_wipe(c->seed, 2 * sizeof *c->seed);
    c->alloc.free_fn(c->alloc.ctx, c->seed);
    c->seed = NULL;
    return true;
}

bool proven_tls_compact_(proven_tls_conn_t *c) {
    if (!c || !c->live || c->dead || !c->established || c->hs || c->cbc || is_quic(c)) return false;
    proven_result_mem_mut_t m = c->alloc.alloc_fn(c->alloc.ctx, 2 * sizeof(proven_tls_key_seed_t), 16);
    if (m.err != PROVEN_OK) return false;          /* no memory to keep the seeds in: stays as it is */
    c->seed = (proven_tls_key_seed_t *)m.value.ptr;
    proven_tls_seed_take_(&c->seed[0], &c->live->read);
    proven_tls_seed_take_(&c->seed[1], &c->live->write);
    tls_wipe(c->live, sizeof *c->live);
    c->alloc.free_fn(c->alloc.ctx, c->live);
    c->live = NULL;
    return true;
}

bool proven_tls_is_compact_(const proven_tls_conn_t *c) { return c && !c->live; }

void proven_tls_conn_destroy(proven_tls_conn_t *c) {
    if (!c) return;
    proven_allocator_t a = c->alloc;
    hs_free(c);
    buf_free(c, &c->in); buf_free(c, &c->out); buf_free(c, &c->post); buf_free(c, &c->peer_cert);
#ifndef PROVEN_NO_HTTP3
    if (c->quic) {
        for (int i = 0; i < PROVEN_QUIC_LEVELS; ++i) buf_free(c, &c->quic->out[i]);
        tls_wipe(c->quic, sizeof *c->quic);
        a.free_fn(a.ctx, c->quic);
    }
#endif
    if (c->server_name) a.free_fn(a.ctx, c->server_name);
    if (c->cbc) { tls_wipe(c->cbc, 2 * sizeof *c->cbc); a.free_fn(a.ctx, c->cbc); }
    if (c->live) { tls_wipe(c->live, sizeof *c->live); a.free_fn(a.ctx, c->live); }
    if (c->seed) { tls_wipe(c->seed, 2 * sizeof *c->seed); a.free_fn(a.ctx, c->seed); }
    tls_wipe(c, sizeof *c);
    a.free_fn(a.ctx, c);
}

proven_err_t proven_tls_feed(proven_tls_conn_t *c, proven_mem_view_t in, proven_size_t *consumed) {
    if (consumed) *consumed = 0;
    if (!c || !consumed || (in.size > 0 && !in.ptr)) return PROVEN_ERR_INVALID_ARG;
    if (is_quic(c)) return PROVEN_ERR_INVALID_STATE;              /* QUIC mode has no records: proven_tls_quic_feed_ */
    if (c->dead) {
        if (!c->error_reported) { c->error_reported = true; return c->error; }
        return PROVEN_ERR_INVALID_STATE;
    }
    if ((in.size > 0 || c->in.len > 0) && !keys_live(c)) { c->error_reported = true; return c->error; }
    proven_size_t used = 0;
    while (!c->dead) {
        if (c->plain_len > c->plain_off) break;                   /* application data is waiting to be read */
        if (c->plain_len > 0) { c->plain_len = 0; c->plain_off = 0; c->in.len = 0; }
        if (c->close_received) { used = in.size; break; }         /* nothing after close_notify means anything */
        if (c->in.len < PROVEN_TLS_RECORD_HEADER) {
            proven_size_t want = PROVEN_TLS_RECORD_HEADER - c->in.len, take = in.size - used < want ? in.size - used : want;
            if (take == 0) break;
            if (!buf_append(c, &c->in, in.ptr + used, take)) { (void)tls_nomem(c); break; }
            used += take;
            if (c->in.len < PROVEN_TLS_RECORD_HEADER) break;
        }
        proven_byte_t type = c->in.ptr[0];
        proven_size_t len = ((proven_size_t)c->in.ptr[3] << 8) | c->in.ptr[4];
        /* The header is checked before a byte of the body is waited for. */
        if (type < PROVEN_TLS_CT_CCS || type > PROVEN_TLS_CT_APPLICATION || c->in.ptr[1] != 0x03) { (void)tls_unexpected(c); break; }
        if (len > PROVEN_TLS_MAX_CIPHERTEXT || (len == 0 && type != PROVEN_TLS_CT_APPLICATION)) { (void)tls_fail(c, AL_RECORD_OVERFLOW, PROVEN_ERR_PROTOCOL); break; }
        proven_size_t need = PROVEN_TLS_RECORD_HEADER + len - c->in.len;
        proven_size_t take = in.size - used < need ? in.size - used : need;
        if (take > 0 && !buf_append(c, &c->in, in.ptr + used, take)) { (void)tls_nomem(c); break; }
        used += take;
        if (c->in.len < PROVEN_TLS_RECORD_HEADER + len) break;
        (void)record_process(c);
        if (c->plain_len == 0) c->in.len = 0;
    }
    *consumed = used;
    if (c->in.len == 0 && c->plain_len == 0) buf_free(c, &c->in);
    if (c->dead) { c->error_reported = true; return c->error; }
    return PROVEN_OK;
}

proven_mem_view_t proven_tls_pending_output(proven_tls_conn_t *c) {
    if (!c || is_quic(c) || c->out.len <= c->out_off) return (proven_mem_view_t){ .ptr = NULL, .size = 0 };
    return (proven_mem_view_t){ .ptr = c->out.ptr + c->out_off, .size = c->out.len - c->out_off };
}

void proven_tls_output_sent(proven_tls_conn_t *c, proven_size_t n) {
    if (!c) return;
    proven_size_t left = c->out.len - c->out_off;
    c->out_off += n < left ? n : left;
    if (c->out_off == c->out.len) { buf_free(c, &c->out); c->out_off = 0; }
}

bool proven_tls_is_established(const proven_tls_conn_t *c) { return c && c->established && !c->dead; }

proven_result_size_t proven_tls_read(proven_tls_conn_t *c, proven_mem_mut_t dest) {
    if (!c || (dest.size > 0 && !dest.ptr)) return (proven_result_size_t){ .err = PROVEN_ERR_INVALID_ARG, .value = 0 };
    if (is_quic(c)) return (proven_result_size_t){ .err = PROVEN_ERR_INVALID_STATE, .value = 0 };
    if (c->plain_len > c->plain_off) {
        proven_size_t have = c->plain_len - c->plain_off, n = have < dest.size ? have : dest.size;
        proven_sys_mem_copy(dest.ptr, c->in.ptr + c->plain_off, n);
        c->plain_off += n;
        c->read_total = n > 0xffffffffu - c->read_total ? 0xffffffffu : c->read_total + (proven_u32)n;
        if (c->in_early) c->early_read = (proven_u16)(c->early_read + n > 0xffffu ? 0xffffu : c->early_read + n);
        if (c->plain_off == c->plain_len) { c->plain_off = 0; c->plain_len = 0; buf_free(c, &c->in); }
        return (proven_result_size_t){ .err = PROVEN_OK, .value = n };
    }
    if (c->dead) return (proven_result_size_t){ .err = PROVEN_ERR_INVALID_STATE, .value = 0 };
    if (c->close_received) return (proven_result_size_t){ .err = PROVEN_ERR_EOF, .value = 0 };
    return (proven_result_size_t){ .err = PROVEN_ERR_NEED_MORE, .value = 0 };
}

/* Whether application data may be written now: once established; by a server that took early
 * data, from its Finished on (0.5-RTT, RFC 8446 section 4.4.4); by a client that offered early
 * data, until the server's hello, as early data (RFC-0027). */
static bool may_write(const proven_tls_conn_t *c) {
    if (c->established) return true;
    if (!c->hs || is_quic(c) || c->v12) return false;
    if (c->is_server) return c->early == PROVEN_TLS_EARLY_ACCEPTED && (c->state == ST_S_WAIT_EOED || c->state == ST_S_WAIT_FINISHED);
    return c->early == PROVEN_TLS_EARLY_OFFERED && c->state == ST_C_WAIT_SH;
}

proven_result_size_t proven_tls_write(proven_tls_conn_t *c, proven_mem_view_t src) {
    if (!c || (src.size > 0 && !src.ptr)) return (proven_result_size_t){ .err = PROVEN_ERR_INVALID_ARG, .value = 0 };
    if (c->dead || !may_write(c) || c->close_sent || is_quic(c)) return (proven_result_size_t){ .err = PROVEN_ERR_INVALID_STATE, .value = 0 };
    if (src.size > 0 && !keys_live(c)) return (proven_result_size_t){ .err = PROVEN_ERR_NOMEM, .value = 0 };
    const bool early = !c->established && !c->is_server;
    proven_size_t done = 0;
    while (done < src.size) {
        proven_size_t unsent = c->out.len - c->out_off;
        if (unsent >= TLS_MAX_UNSENT) break;
        proven_size_t n = src.size - done < c->peer_record_limit ? src.size - done : c->peer_record_limit;
        if (early) {
            /* Early data stops at what the ticket allows; the rest waits for the handshake. */
            if (c->hs->early_left == 0) break;
            if (n > c->hs->early_left) n = c->hs->early_left;
            if (!emit(c, PROVEN_TLS_CT_APPLICATION, src.ptr + done, n, 0x03)) return (proven_result_size_t){ .err = PROVEN_ERR_NOMEM, .value = done };
            c->hs->early_left -= (proven_u32)n;
            c->hs->early_written += (proven_u32)n;
            done += n;
            continue;
        }
        /* Long before a key has sealed 2^24 full records it is replaced: far inside every
         * suite's limit on how much one key may protect. */
        if (c->v12) {
            /* TLS 1.2 cannot replace a key in place. Far below any suite's limit, this side stops
             * writing: a connection that has carried that much is closed and made again. */
            if (c->live->write.seq >= ((proven_u64)1 << 31)) return (proven_result_size_t){ .err = done ? PROVEN_OK : PROVEN_ERR_OVERFLOW, .value = done };
        } else if (c->live->write.seq >= ((proven_u64)1 << 24) && !key_update_send(c, 0)) return (proven_result_size_t){ .err = PROVEN_ERR_NOMEM, .value = done };
        if (!emit(c, PROVEN_TLS_CT_APPLICATION, src.ptr + done, n, 0x03)) return (proven_result_size_t){ .err = PROVEN_ERR_NOMEM, .value = done };
        done += n;
    }
    return (proven_result_size_t){ .err = PROVEN_OK, .value = done };
}

proven_result_size_t proven_tls_seal_into_(proven_tls_conn_t *c, proven_mem_view_t src, proven_mem_mut_t out, proven_size_t *written) {
    if (written) *written = 0;
    if (!c || !written || (src.size > 0 && !src.ptr) || (out.size > 0 && !out.ptr)) return (proven_result_size_t){ .err = PROVEN_ERR_INVALID_ARG, .value = 0 };
    if (c->dead || !may_write(c) || (!c->established && !c->is_server) || c->close_sent || is_quic(c)) return (proven_result_size_t){ .err = PROVEN_ERR_INVALID_STATE, .value = 0 };
    if (c->cbc) return (proven_result_size_t){ .err = PROVEN_ERR_UNSUPPORTED, .value = 0 };      /* its random IVs and split records: proven_tls_write */
    if (src.size > 0 && !keys_live(c)) return (proven_result_size_t){ .err = PROVEN_ERR_NOMEM, .value = 0 };
    proven_size_t done = 0, at = 0;
    /* An AEAD record: the header, and TLS 1.3's inner type and tag, or TLS 1.2's explicit nonce
     * and tag - 29 bytes at most, so that a whole record of 2^14 fits a loop's send chunk. */
    const proven_size_t overhead = PROVEN_TLS_AEAD_MAX_RECORD_EXTRA;
    /* The engine's own records go first: while it holds some, nothing is sealed here. */
    while (done < src.size && c->out.len == c->out_off) {
        if (c->v12) {
            if (c->live->write.seq >= ((proven_u64)1 << 31)) return (proven_result_size_t){ .err = done ? PROVEN_OK : PROVEN_ERR_OVERFLOW, .value = done };
        } else if (c->live->write.seq >= ((proven_u64)1 << 24)) {
            if (!key_update_send(c, 0)) return (proven_result_size_t){ .err = PROVEN_ERR_NOMEM, .value = done };
            break;                                 /* the KeyUpdate waits in the engine: it leaves first */
        }
        const proven_size_t room = out.size - at;
        proven_size_t n = src.size - done < c->peer_record_limit ? src.size - done : c->peer_record_limit;
        if (room < overhead + 1) break;
        if (n > room - overhead) {
            n = room - overhead;
            if (n < 256 && n < src.size - done) break;      /* not worth a record of its own: the next buffer */
        }
        proven_byte_t *o = out.ptr + at;
        at += c->v12 ? proven_tls12_seal(&c->live->write, PROVEN_TLS_CT_APPLICATION, (proven_mem_view_t){ .ptr = src.ptr + done, .size = n }, o)
                     : proven_tls13_seal(&c->live->write, PROVEN_TLS_CT_APPLICATION, (proven_mem_view_t){ .ptr = src.ptr + done, .size = n }, o);
        done += n;
    }
    *written = at;
    return (proven_result_size_t){ .err = PROVEN_OK, .value = done };
}

proven_err_t proven_tls_close(proven_tls_conn_t *c) {
    if (!c) return PROVEN_ERR_INVALID_ARG;
    if (c->dead || is_quic(c)) return PROVEN_ERR_INVALID_STATE;     /* QUIC closes with CONNECTION_CLOSE, not an alert */
    if (c->close_sent) return PROVEN_OK;
    if (!keys_live(c)) return PROVEN_ERR_NOMEM;
    proven_byte_t body[2] = { 1, AL_CLOSE_NOTIFY };
    c->close_sent = true;
    return emit(c, PROVEN_TLS_CT_ALERT, body, 2, 0x03) ? PROVEN_OK : PROVEN_ERR_NOMEM;
}

proven_err_t proven_tls_key_update(proven_tls_conn_t *c) {
    if (!c) return PROVEN_ERR_INVALID_ARG;
    if (c->dead || !c->established || c->close_sent) return PROVEN_ERR_INVALID_STATE;
    if (c->v12 || is_quic(c)) return PROVEN_ERR_UNSUPPORTED;      /* TLS 1.2 has no such message, and QUIC forbids it */
    if (!keys_live(c)) return PROVEN_ERR_NOMEM;
    return key_update_send(c, 0) ? PROVEN_OK : PROVEN_ERR_NOMEM;
}

proven_u16 proven_tls_cipher_suite(const proven_tls_conn_t *c) { return !c ? 0 : c->suite12 ? c->suite12->id : c->suite ? c->suite->id : 0; }
proven_u16 proven_tls_version(const proven_tls_conn_t *c) { return c ? c->version : 0; }

proven_u8str_view_t proven_tls_alpn(const proven_tls_conn_t *c) {
    if (!c || c->alpn_index < 0) return (proven_u8str_view_t){ .ptr = NULL, .size = 0 };
    return proven_tls_config_alpn(c->config, (proven_size_t)c->alpn_index);
}

bool proven_tls_resumed(const proven_tls_conn_t *c) { return c && c->resumed; }
bool proven_tls_ticket_stored_(const proven_tls_conn_t *c) { return c && c->ticket_stored; }

bool proven_tls_peer_key_sha256(const proven_tls_conn_t *c, proven_byte_t out[32]) {
    if (!c || !out || !c->has_peer_key) return false;
    for (int i = 0; i < 32; ++i) out[i] = c->peer_key_hash[i];
    return true;
}

proven_mem_view_t proven_tls_peer_certificate(const proven_tls_conn_t *c) {
    if (!c) return (proven_mem_view_t){ .ptr = NULL, .size = 0 };
    return (proven_mem_view_t){ .ptr = c->peer_cert.ptr, .size = c->peer_cert.len };
}

proven_cert_fault_t proven_tls_peer_fault(const proven_tls_conn_t *c) { return c ? c->peer_fault : PROVEN_CERT_FAULT_NONE; }

proven_u8str_view_t proven_tls_server_name(const proven_tls_conn_t *c) {
    if (!c || !c->is_server || !c->hs) return (proven_u8str_view_t){ .ptr = NULL, .size = 0 };
    return (proven_u8str_view_t){ .ptr = c->hs->sni, .size = c->hs->sni_len };
}

int proven_tls_alert_received(const proven_tls_conn_t *c) { return c ? c->alert_received : -1; }
int proven_tls_alert_sent(const proven_tls_conn_t *c) { return c ? c->alert_sent : -1; }

void proven_tls_test_records_(proven_tls_conn_t *writer, proven_tls_conn_t *reader, proven_u64 count) {
    if (writer && keys_live(writer)) writer->live->write.seq = count;
    if (reader && keys_live(reader)) reader->live->read.seq = count;
}

/* ---- 0-RTT over TCP (RFC-0027): proven_internal_tls.h ---- */

void proven_tls_server_take_early_(proven_tls_conn_t *c) { if (c && c->is_server && !is_quic(c)) c->take_early = true; }

int proven_tls_early_(const proven_tls_conn_t *c) {
    if (!c) return PROVEN_TLS_EARLY_NONE;
#ifndef PROVEN_NO_HTTP3
    if (c->quic) return c->quic->early;
#endif
    return c->early;
}

proven_size_t proven_tls_early_written_(const proven_tls_conn_t *c) { return c && c->hs && !c->is_server ? c->hs->early_written : 0; }

proven_size_t proven_tls_early_room_(const proven_tls_conn_t *c) {
    return c && c->hs && !c->is_server && c->early == PROVEN_TLS_EARLY_OFFERED && c->state == ST_C_WAIT_SH ? c->hs->early_left : 0;
}

bool proven_tls_pending_early_(const proven_tls_conn_t *c) { return c && c->in_early && c->plain_len > c->plain_off; }

bool proven_tls_writable_(const proven_tls_conn_t *c) { return c && !c->dead && !c->close_sent && may_write(c) && (c->established || c->is_server); }

bool proven_tls_session_alpn_is_(const proven_tls_session_t *session, proven_u8str_view_t name) {
    if (!session) return false;
    const proven_tls_session_data_t *s = (const proven_tls_session_data_t *)(const void *)session->opaque;
    return s->magic == PROVEN_TLS_SESSION_MAGIC && s->alpn_len == name.size && bytes_eq(s->alpn, name.ptr, name.size);
}

void proven_tls_read_counts_(const proven_tls_conn_t *c, proven_size_t *early, proven_size_t *total) { *early = c ? c->early_read : 0; *total = c ? c->read_total : 0; }

bool proven_tls_is_server_(const proven_tls_conn_t *c) { return c && c->is_server; }

bool proven_tls_session_allows_early_(const proven_tls_session_t *session) {
    if (!session) return false;
    const proven_tls_session_data_t *s = (const proven_tls_session_data_t *)(const void *)session->opaque;
    return s->magic == PROVEN_TLS_SESSION_MAGIC && s->early && s->early_max > 0;
}
