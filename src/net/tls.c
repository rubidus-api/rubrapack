// src/net/tls.c - a TLS 1.3 client (RFC 8446; include/rubrapack/tls.h). The protocol's
// structure follows lowent's tls13/tlscli modules (the same owner's code, read at tag v1.3.0),
// widened as RFC-0008 T3 asks: two cipher suites, two groups with HelloRetryRequest, RSA-PSS and
// ECDSA server signatures, and the certificate checks (path, validity, name, purpose).
//
// Every length in a received message is the peer's claim: each read is bounded by what is left of
// its own container, and nothing is repaired. Records are at most 2^14 bytes of plaintext (plus
// 256 when encrypted); a handshake message is at most 64 KiB; a certificate chain at most 10
// certificates.

#include "rubrapack/tls.h"

#include "rubrapack/buf.h"
#include "rubrapack/mem.h"
#include "rubrapack/pki.h"
#include "rubrapack/sign.h"

#include <stdio.h>
#include <string.h>

#include "proven/random.h"

enum {
    CT_CCS = 20, CT_ALERT = 21, CT_HANDSHAKE = 22, CT_APP = 23,
    HS_CLIENT_HELLO = 1, HS_SERVER_HELLO = 2, HS_NEW_SESSION_TICKET = 4, HS_ENCRYPTED_EXTENSIONS = 8, HS_CERTIFICATE = 11,
    HS_CERTIFICATE_REQUEST = 13, HS_CERTIFICATE_VERIFY = 15, HS_FINISHED = 20, HS_KEY_UPDATE = 24, HS_MESSAGE_HASH = 254,
    SUITE_AES128 = 0x1301, SUITE_AES256 = 0x1302, GROUP_X25519 = 0x001D, GROUP_P256 = 0x0017,
    EXT_SNI = 0x0000, EXT_GROUPS = 0x000A, EXT_SIGALGS = 0x000D, EXT_VERSIONS = 0x002B, EXT_COOKIE = 0x002C, EXT_KEY_SHARE = 0x0033,
    MAX_HS = 65536, MAX_CERTS = 10,
    // alerts (RFC 8446 6)
    AL_CLOSE = 0, AL_UNEXPECTED = 10, AL_BAD_MAC = 20, AL_OVERFLOW = 22, AL_HANDSHAKE = 40, AL_BAD_CERT = 42, AL_UNSUPPORTED_CERT = 43,
    AL_EXPIRED = 45, AL_UNKNOWN_CA = 48, AL_ILLEGAL = 47, AL_DECODE = 50, AL_DECRYPT = 51, AL_PROTOCOL_VERSION = 70,
    AL_INTERNAL = 80, AL_MISSING_EXT = 109, AL_UNSUPPORTED_EXT = 110,
};

// HelloRetryRequest's fixed ServerHello.random: SHA-256("HelloRetryRequest") (RFC 8446 4.1.3).
static const uint8_t HRR_RANDOM[32] = { 0xCF, 0x21, 0xAD, 0x74, 0xE5, 0x9A, 0x61, 0x11, 0xBE, 0x1D, 0x8C, 0x02, 0x1E, 0x65, 0xB8, 0x91,
                                        0xC2, 0xA2, 0x11, 0x16, 0x7A, 0xBB, 0x8C, 0x5E, 0x07, 0x9E, 0x09, 0xE2, 0xC8, 0xA8, 0x33, 0x9C };

// ---- key schedule --------------------------------------------------------------------------------

bool rp_tls_expand_label(rp_hash_alg_t alg, const uint8_t *secret, const char *label, const uint8_t *ctx, size_t ctx_len, uint8_t *out,
                         size_t len) {
    size_t ll = strlen(label);
    if (ll > 249 || ctx_len > 255 || len > 65535) return false;
    uint8_t info[2 + 1 + 255 + 1 + 255];
    size_t n = 0;
    info[n++] = (uint8_t)(len >> 8);
    info[n++] = (uint8_t)len;
    info[n++] = (uint8_t)(6 + ll);
    memcpy(info + n, "tls13 ", 6);
    n += 6;
    memcpy(info + n, label, ll);
    n += ll;
    info[n++] = (uint8_t)ctx_len;
    if (ctx_len) memcpy(info + n, ctx, ctx_len);
    n += ctx_len;
    return rp_hkdf_expand(alg, secret, rp_hash_size(alg), info, n, out, len);
}

// Derive-Secret(secret, label, transcript hash).
static void derive(rp_hash_alg_t alg, const uint8_t *secret, const char *label, const uint8_t *th, uint8_t *out) {
    size_t hl = rp_hash_size(alg);
    bool ok = rp_tls_expand_label(alg, secret, label, th, hl, out, hl);
    (void)ok;                                           // fixed, short labels: cannot fail
}

void rp_tls_keys_handshake(rp_tls_keys_t *k, rp_hash_alg_t alg, size_t key_len, const uint8_t *shared, size_t shared_len,
                           const uint8_t *th) {
    memset(k, 0, sizeof *k);
    k->alg = alg;
    k->hl = rp_hash_size(alg);
    k->key_len = key_len;
    uint8_t zero[RP_HASH_MAX] = { 0 }, early[RP_HASH_MAX], empty[RP_HASH_MAX], derived[RP_HASH_MAX];
    rp_hkdf_extract(alg, NULL, 0, zero, k->hl, early);
    rp_hash(alg, "", 0, empty);
    derive(alg, early, "derived", empty, derived);
    rp_hkdf_extract(alg, derived, k->hl, shared, shared_len, k->hs);
    derive(alg, k->hs, "c hs traffic", th, k->c_hs);
    derive(alg, k->hs, "s hs traffic", th, k->s_hs);
    derive(alg, k->hs, "derived", empty, derived);
    rp_hkdf_extract(alg, derived, k->hl, zero, k->hl, k->master);
    rp_wipe(early, sizeof early);
    rp_wipe(derived, sizeof derived);
}

void rp_tls_keys_app(rp_tls_keys_t *k, const uint8_t *th) {
    derive(k->alg, k->master, "c ap traffic", th, k->c_ap);
    derive(k->alg, k->master, "s ap traffic", th, k->s_ap);
}

void rp_tls_finished(rp_hash_alg_t alg, const uint8_t *base_key, const uint8_t *th, uint8_t *out) {
    size_t hl = rp_hash_size(alg);
    uint8_t fk[RP_HASH_MAX];
    bool ok = rp_tls_expand_label(alg, base_key, "finished", NULL, 0, fk, hl);
    (void)ok;
    rp_hmac(alg, fk, hl, th, hl, out);
    rp_wipe(fk, sizeof fk);
}

// ---- records --------------------------------------------------------------------------------------

bool rp_tls_aead_init(rp_tls_aead_t *a, rp_hash_alg_t alg, size_t key_len, const uint8_t *secret) {
    uint8_t key[32];
    memset(a, 0, sizeof *a);
    bool ok = rp_tls_expand_label(alg, secret, "key", NULL, 0, key, key_len) && rp_tls_expand_label(alg, secret, "iv", NULL, 0, a->iv, 12) &&
              rp_gcm_init(&a->gcm, key, key_len);
    rp_wipe(key, sizeof key);
    return ok;
}

static void nonce(const rp_tls_aead_t *a, uint8_t n[12]) {
    memcpy(n, a->iv, 12);
    for (int i = 0; i < 8; ++i) n[4 + i] ^= (uint8_t)(a->seq >> (56 - 8 * i));
}

void rp_tls_seal(rp_tls_aead_t *a, uint8_t type, const uint8_t *in, size_t len, uint8_t *out, size_t *out_len) {
    size_t ct = len + 1 + 16;
    out[0] = CT_APP;
    out[1] = 0x03;
    out[2] = 0x03;
    out[3] = (uint8_t)(ct >> 8);
    out[4] = (uint8_t)ct;
    memmove(out + 5, in, len);
    out[5 + len] = type;
    uint8_t n[12];
    nonce(a, n);
    rp_gcm_seal(&a->gcm, n, out, 5, out + 5, len + 1, out + 5, out + 5 + len + 1);
    ++a->seq;
    *out_len = 5 + ct;
}

bool rp_tls_open(rp_tls_aead_t *a, const uint8_t *rec, size_t rec_len, uint8_t *out, size_t *out_len, uint8_t *type) {
    if (rec_len < 5 + 17 || rec[0] != CT_APP || rec_len - 5 > RP_TLS_MAX_CIPHER || (size_t)((rec[3] << 8) | rec[4]) != rec_len - 5) return false;
    uint8_t n[12];
    nonce(a, n);
    size_t cl = rec_len - 5 - 16;
    if (!rp_gcm_open(&a->gcm, n, rec, 5, rec + 5, cl, rec + rec_len - 16, out)) return false;
    ++a->seq;
    while (cl && out[cl - 1] == 0) --cl;                // padding: zeros after the real type
    if (cl == 0 || cl - 1 > RP_TLS_MAX_PLAIN) return false;
    *type = out[cl - 1];
    *out_len = cl - 1;
    return true;
}

// ---- CertificateVerify -------------------------------------------------------------------------------

bool rp_tls_cv_check(const uint8_t *leaf, size_t leaf_len, uint16_t scheme, const uint8_t *sig, size_t sig_len, const uint8_t *th,
                     size_t hl, const char **why) {
    rp_cert_t c;
    if (!rp_cert_parse(leaf, leaf_len, &c, why)) return false;
    rp_hash_alg_t alg;
    bool pss = false;
    int curve = -1;
    switch (scheme) {
    case 0x0804: alg = RP_HASH_SHA256, pss = true; break;
    case 0x0805: alg = RP_HASH_SHA384, pss = true; break;
    case 0x0806: alg = RP_HASH_SHA512, pss = true; break;
    case 0x0403: alg = RP_HASH_SHA256, curve = RP_EC_P256; break;
    case 0x0503: alg = RP_HASH_SHA384, curve = RP_EC_P384; break;
    default:
        *why = (scheme & 0xFF) == 0x01 ? "the server signed with RSASSA-PKCS1, which TLS 1.3 forbids in CertificateVerify"
                                       : "the server signed with a scheme rubrapack does not offer";
        return false;
    }
    if (pss ? !c.rsa : c.ec_curve != curve) {
        *why = "the server's signature scheme does not fit its certificate's key";
        return false;
    }
    // 64 spaces, the context string, a zero byte, the transcript hash (RFC 8446 4.4.3).
    static const char ctx[] = "TLS 1.3, server CertificateVerify";
    uint8_t content[64 + sizeof ctx + RP_HASH_MAX], d[RP_HASH_MAX];
    memset(content, 0x20, 64);
    memcpy(content + 64, ctx, sizeof ctx);              // with its terminating zero
    memcpy(content + 64 + sizeof ctx, th, hl);
    rp_hash(alg, content, 64 + sizeof ctx + hl, d);
    bool ok = pss ? rp_rsa_pss_verify(c.rsa_n.p, c.rsa_n.n, c.rsa_e.p, c.rsa_e.n, alg, d, sig, sig_len, rp_hash_size(alg))
                  : rp_cert_verify_sig(&c, alg, d, sig, sig_len);
    if (!ok) *why = "the server's CertificateVerify signature does not verify";
    return ok;
}

// ---- the client ---------------------------------------------------------------------------------------

typedef enum { ST_WAIT_SH, ST_WAIT_EE, ST_WAIT_CERT, ST_WAIT_CV, ST_WAIT_FIN, ST_CONNECTED, ST_FAILED } state_t;

struct rp_tls {
    proven_allocator_t alloc;
    char               host[256];
    bool               host_is_ip;
    const rp_der_span_t *anchors;
    size_t             anchor_count;
    int64_t            now;
    state_t            st;
    uint8_t            random[32], sid[32], x_priv[32], x_pub[32], p_priv[32], p_pub[65];
    uint16_t           group;                   // the group of the key share sent last
    bool               hrr;                     // a HelloRetryRequest came
    uint16_t           suite;
    uint8_t           *cookie;
    size_t             cookie_len;
    bool               ccs_sent;
    rp_tls_keys_t      keys;
    rp_tls_aead_t      rd, wr;
    bool               rd_on, wr_on, eof;
    rp_buf_t           tr;                      // the transcript (handshake messages as sent and received)
    rp_buf_t           in;                      // received bytes not yet a whole record
    rp_buf_t           hs;                      // handshake bytes not yet a whole message
    rp_buf_t           out;                     // bytes to send
    size_t             out_pos;
    rp_buf_t           app;                     // received application data
    size_t             app_pos;
    rp_buf_t           chain;                   // the server's certificates, DER, one after the other
    size_t             leaf_len;
    uint8_t            rec[5 + RP_TLS_MAX_CIPHER];
};

static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t be24(const uint8_t *p) { return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2]; }
static void put16(rp_buf_t *b, size_t v) {
    rp_buf_byte(b, (uint8_t)(v >> 8));
    rp_buf_byte(b, (uint8_t)v);
}

// Sets *why, queues a fatal alert and ends the connection.
static proven_err_t fail(rp_tls_t *t, uint8_t alert, const char *msg, const char **why) {
    *why = msg;
    if (t->st != ST_FAILED) {
        uint8_t a[2] = { 2, alert };
        if (t->wr_on) {
            size_t n;
            rp_tls_seal(&t->wr, CT_ALERT, a, 2, t->rec, &n);
            rp_buf_put(&t->out, t->rec, n);
        } else {
            uint8_t r[7] = { CT_ALERT, 3, 3, 0, 2, a[0], a[1] };
            rp_buf_put(&t->out, r, sizeof r);
        }
    }
    t->st = ST_FAILED;
    return PROVEN_ERR_INVALID_FORMAT;
}

static void th(const rp_tls_t *t, uint8_t *out) { rp_hash(t->keys.alg, t->tr.data, t->tr.len, out); }

// Queues handshake message `msg` (header included) in a plaintext or protected record.
static void send_hs(rp_tls_t *t, const uint8_t *msg, size_t len, bool first) {
    if (t->wr_on) {
        size_t n;
        rp_tls_seal(&t->wr, CT_HANDSHAKE, msg, len, t->rec, &n);
        rp_buf_put(&t->out, t->rec, n);
    } else {
        uint8_t h[5] = { CT_HANDSHAKE, 3, first ? 1 : 3, (uint8_t)(len >> 8), (uint8_t)len };
        rp_buf_put(&t->out, h, 5);
        rp_buf_put(&t->out, msg, len);
    }
}

static void send_ccs(rp_tls_t *t) {
    if (t->ccs_sent) return;
    static const uint8_t ccs[6] = { CT_CCS, 3, 3, 0, 1, 1 };  // middlebox compatibility (RFC 8446 D.4)
    rp_buf_put(&t->out, ccs, sizeof ccs);
    t->ccs_sent = true;
}

static bool client_hello(rp_tls_t *t, bool first) {
    rp_buf_t e = rp_buf_new(t->alloc, 1u << 17), m = rp_buf_new(t->alloc, 1u << 17);
    if (!t->host_is_ip) {                              // server_name (RFC 6066 3): DNS names only
        size_t hl = strlen(t->host);
        put16(&e, EXT_SNI);
        put16(&e, hl + 5);
        put16(&e, hl + 3);
        rp_buf_byte(&e, 0);
        put16(&e, hl);
        rp_buf_put(&e, t->host, hl);
    }
    static const uint8_t groups[] = { 0x00, 0x0A, 0x00, 0x06, 0x00, 0x04, 0x00, 0x1D, 0x00, 0x17 };
    rp_buf_put(&e, groups, sizeof groups);
    // CertificateVerify takes PSS and ECDSA; the certificate path may also use PKCS#1.
    static const uint8_t sigalgs[] = { 0x00, 0x0D, 0x00, 0x12, 0x00, 0x10, 0x04, 0x03, 0x05, 0x03, 0x08, 0x04, 0x08, 0x05,
                                       0x08, 0x06, 0x04, 0x01, 0x05, 0x01, 0x06, 0x01 };
    rp_buf_put(&e, sigalgs, sizeof sigalgs);
    static const uint8_t versions[] = { 0x00, 0x2B, 0x00, 0x03, 0x02, 0x03, 0x04 };
    rp_buf_put(&e, versions, sizeof versions);
    if (t->cookie_len) {
        put16(&e, EXT_COOKIE);
        put16(&e, t->cookie_len + 2);
        put16(&e, t->cookie_len);
        rp_buf_put(&e, t->cookie, t->cookie_len);
    }
    bool x = t->group == GROUP_X25519;
    size_t kl = x ? 32 : 65;
    put16(&e, EXT_KEY_SHARE);
    put16(&e, kl + 6);
    put16(&e, kl + 4);
    put16(&e, t->group);
    put16(&e, kl);
    rp_buf_put(&e, x ? t->x_pub : t->p_pub, kl);
    size_t body = 2 + 32 + 1 + 32 + 2 + 4 + 2 + 2 + e.len;
    rp_buf_byte(&m, HS_CLIENT_HELLO);
    rp_buf_byte(&m, (uint8_t)(body >> 16));
    put16(&m, body);
    put16(&m, 0x0303);
    rp_buf_put(&m, t->random, 32);
    rp_buf_byte(&m, 32);
    rp_buf_put(&m, t->sid, 32);
    static const uint8_t suites[] = { 0x00, 0x04, 0x13, 0x01, 0x13, 0x02, 0x01, 0x00 };
    rp_buf_put(&m, suites, sizeof suites);
    put16(&m, e.len);
    rp_buf_put(&m, e.data, e.len);
    bool ok = e.err == PROVEN_OK && m.err == PROVEN_OK && m.len <= RP_TLS_MAX_PLAIN;
    if (ok) {
        if (!first) send_ccs(t);
        send_hs(t, m.data, m.len, first);
        rp_buf_put(&t->tr, m.data, m.len);
    }
    rp_buf_free(&e);
    rp_buf_free(&m);
    return ok && t->out.err == PROVEN_OK && t->tr.err == PROVEN_OK;
}

proven_err_t rp_tls_new(proven_allocator_t alloc, const rp_tls_config_t *cfg, rp_tls_t **out, const char **why) {
    *out = NULL;
    size_t hl = cfg->host ? strlen(cfg->host) : 0;
    if (hl == 0 || hl >= 256) {
        *why = "no server name, or one longer than 255 bytes";
        return PROVEN_ERR_INVALID_ARG;
    }
    rp_tls_t *t = rp_mem_alloc(alloc, 1, sizeof *t);
    if (t == NULL) return PROVEN_ERR_NOMEM;
    memset(t, 0, sizeof *t);
    t->alloc = alloc;
    memcpy(t->host, cfg->host, hl);
    if (t->host[hl - 1] == '.') t->host[hl - 1] = 0;  // "example.com." names example.com
    uint8_t ip[16];
    t->host_is_ip = rp_ip_parse(t->host, ip) != 0;
    t->anchors = cfg->anchors;
    t->anchor_count = cfg->anchor_count;
    t->now = cfg->now;
    t->tr = rp_buf_new(alloc, 1u << 18);
    t->in = rp_buf_new(alloc, 1u << 16);
    t->hs = rp_buf_new(alloc, MAX_HS + 4);
    t->out = rp_buf_new(alloc, 1u << 20);
    t->app = rp_buf_new(alloc, 1u << 24);
    t->chain = rp_buf_new(alloc, MAX_HS);
    uint8_t rnd[128];
    if (cfg->test_random) memcpy(rnd, cfg->test_random, sizeof rnd);
    else if (!proven_random_bytes(rnd, sizeof rnd)) {
        rp_tls_free(t);
        *why = "the system random source failed";
        return PROVEN_ERR_IO;
    }
    memcpy(t->random, rnd, 32);
    memcpy(t->sid, rnd + 32, 32);
    memcpy(t->x_priv, rnd + 64, 32);
    memcpy(t->p_priv, rnd + 96, 32);
    rp_wipe(rnd, sizeof rnd);
    static const uint8_t base[32] = { 9 };
    if (!rp_x25519(t->x_pub, t->x_priv, base)) {
        rp_tls_free(t);
        *why = "cannot make a key share";
        return PROVEN_ERR_IO;
    }
    t->group = GROUP_X25519;
    t->st = ST_WAIT_SH;
    if (!client_hello(t, true)) {
        rp_tls_free(t);
        *why = "cannot build the ClientHello";
        return PROVEN_ERR_NOMEM;
    }
    *out = t;
    return PROVEN_OK;
}

void rp_tls_free(rp_tls_t *t) {
    if (t == NULL) return;
    rp_buf_free(&t->tr);
    rp_buf_free(&t->in);
    rp_buf_free(&t->hs);
    rp_buf_free(&t->out);
    rp_buf_free(&t->app);
    rp_buf_free(&t->chain);
    if (t->cookie) rp_mem_free(t->alloc, t->cookie);
    proven_allocator_t a = t->alloc;
    rp_wipe(t, sizeof *t);
    rp_mem_free(a, t);
}

size_t rp_tls_pending(rp_tls_t *t, const uint8_t **data) {
    *data = t->out.len ? t->out.data + t->out_pos : NULL;
    return t->out.len - t->out_pos;
}

void rp_tls_sent(rp_tls_t *t, size_t n) {
    t->out_pos += n;
    if (t->out_pos == t->out.len) t->out_pos = t->out.len = 0;
}

bool rp_tls_connected(const rp_tls_t *t) { return t->st == ST_CONNECTED; }
bool rp_tls_eof(const rp_tls_t *t) { return t->eof; }

// ---- ServerHello and HelloRetryRequest ----------------------------------------------------------------

typedef struct {
    bool     version_ok, has_version, has_share;
    uint16_t share_group;
    const uint8_t *share;
    size_t   share_len;
    const uint8_t *cookie;
    size_t   cookie_len;
} sh_ext_t;

static bool parse_sh_ext(const uint8_t *p, size_t n, bool hrr, sh_ext_t *x, const char **bad) {
    memset(x, 0, sizeof *x);
    uint32_t seen = 0;
    while (n) {
        if (n < 4 || be16(p + 2) > n - 4) return *bad = "a malformed ServerHello extension", false;
        uint16_t type = be16(p), len = be16(p + 2);
        const uint8_t *v = p + 4;
        uint32_t bit = type == EXT_VERSIONS ? 1 : type == EXT_KEY_SHARE ? 2 : type == EXT_COOKIE ? 4 : 0;
        if (bit == 0 || (bit == 4 && !hrr)) return *bad = "the server sent an extension rubrapack did not offer", false;
        if (seen & bit) return *bad = "the server sent an extension twice", false;
        seen |= bit;
        if (type == EXT_VERSIONS) {
            if (len != 2) return *bad = "a malformed supported_versions", false;
            x->has_version = true;
            x->version_ok = be16(v) == 0x0304;
        } else if (type == EXT_KEY_SHARE) {
            if (hrr) {
                if (len != 2) return *bad = "a malformed key_share", false;
                x->share_group = be16(v);
            } else {
                if (len < 4 || be16(v + 2) != len - 4) return *bad = "a malformed key_share", false;
                x->share_group = be16(v);
                x->share = v + 4;
                x->share_len = len - 4u;
            }
            x->has_share = true;
        } else {
            if (len < 3 || be16(v) != len - 2) return *bad = "a malformed cookie", false;
            x->cookie = v + 2;
            x->cookie_len = len - 2u;
        }
        p += 4 + len;
        n -= 4 + (size_t)len;
    }
    return true;
}

static proven_err_t server_hello(rp_tls_t *t, const uint8_t *msg, size_t len, const char **why) {
    const uint8_t *b = msg + 4;
    size_t n = len - 4;
    if (n < 2 + 32 + 1) return fail(t, AL_DECODE, "a malformed ServerHello", why);
    uint8_t sidl = b[34];
    if (n < 35u + sidl + 2 + 1 + 2) return fail(t, AL_DECODE, "a malformed ServerHello", why);
    const uint8_t *p = b + 35 + sidl;
    uint16_t suite = be16(p);
    uint8_t comp = p[2];
    size_t el = be16(p + 3);
    if (el != n - (35u + sidl + 5)) return fail(t, AL_DECODE, "a malformed ServerHello", why);
    bool hrr = memcmp(b + 2, HRR_RANDOM, 32) == 0;
    sh_ext_t x;
    const char *bad = NULL;
    if (!parse_sh_ext(p + 5, el, hrr, &x, &bad)) return fail(t, AL_UNSUPPORTED_EXT, bad, why);
    if (!x.has_version || !x.version_ok || be16(b) != 0x0303) {
        return fail(t, AL_PROTOCOL_VERSION, "the server does not speak TLS 1.3 (TLS 1.2 and older are not supported)", why);
    }
    if (sidl != 32 || memcmp(b + 35, t->sid, 32) != 0) return fail(t, AL_ILLEGAL, "the server did not echo the session id", why);
    if ((suite != SUITE_AES128 && suite != SUITE_AES256) || comp != 0) return fail(t, AL_ILLEGAL, "the server chose a cipher suite rubrapack did not offer", why);
    if (t->hrr && suite != t->suite) return fail(t, AL_ILLEGAL, "the server changed its cipher suite after HelloRetryRequest", why);
    t->suite = suite;
    rp_hash_alg_t alg = suite == SUITE_AES128 ? RP_HASH_SHA256 : RP_HASH_SHA384;
    t->keys.alg = alg;
    size_t hl = rp_hash_size(alg);
    if (t->hs.len) return fail(t, AL_UNEXPECTED, "handshake data after ServerHello in the same record", why);
    if (hrr) {
        if (t->hrr) return fail(t, AL_UNEXPECTED, "a second HelloRetryRequest", why);
        if (!x.has_share || (x.share_group != GROUP_X25519 && x.share_group != GROUP_P256) || x.share_group == t->group) {
            return fail(t, AL_ILLEGAL, "the server's HelloRetryRequest asks for a group rubrapack cannot use", why);
        }
        t->hrr = true;
        // The transcript restarts with message_hash(ClientHello1) (RFC 8446 4.4.1).
        uint8_t h[RP_HASH_MAX], mh[4 + RP_HASH_MAX] = { HS_MESSAGE_HASH, 0, 0, (uint8_t)hl };
        rp_hash(alg, t->tr.data, t->tr.len, h);
        memcpy(mh + 4, h, hl);
        t->tr.len = 0;
        rp_buf_put(&t->tr, mh, 4 + hl);
        rp_buf_put(&t->tr, msg, len);
        if (x.cookie_len) {
            t->cookie = rp_mem_alloc(t->alloc, x.cookie_len, 1);
            if (t->cookie == NULL) return fail(t, AL_INTERNAL, "out of memory", why);
            memcpy(t->cookie, x.cookie, x.cookie_len);
            t->cookie_len = x.cookie_len;
        }
        t->group = x.share_group;
        if (t->group == GROUP_P256) {
            uint8_t px[32], py[32];
            bool ok = false;
            for (int tries = 0; tries < 8 && !(ok = rp_ec_public(RP_EC_P256, t->p_priv, px, py)); ++tries) {
                if (!proven_random_bytes(t->p_priv, 32)) break;
            }
            if (!ok) return fail(t, AL_INTERNAL, "cannot make a P-256 key share", why);
            t->p_pub[0] = 4;
            memcpy(t->p_pub + 1, px, 32);
            memcpy(t->p_pub + 33, py, 32);
        }
        if (!client_hello(t, false)) return fail(t, AL_INTERNAL, "cannot build the second ClientHello", why);
        return PROVEN_OK;
    }
    if (!x.has_share || x.share_group != t->group) return fail(t, AL_ILLEGAL, "the server's key share is not for the group rubrapack offered", why);
    uint8_t shared[32];
    bool ok;
    if (t->group == GROUP_X25519) {
        ok = x.share_len == 32 && rp_x25519(shared, t->x_priv, x.share);
    } else {
        ok = x.share_len == 65 && x.share[0] == 4 && rp_ecdh(RP_EC_P256, t->p_priv, x.share + 1, x.share + 33, shared);
    }
    rp_wipe(t->x_priv, sizeof t->x_priv);
    rp_wipe(t->p_priv, sizeof t->p_priv);
    if (!ok) return fail(t, AL_ILLEGAL, "the server's key share is not a valid point", why);
    rp_buf_put(&t->tr, msg, len);
    uint8_t h[RP_HASH_MAX];
    th(t, h);
    rp_tls_keys_handshake(&t->keys, alg, suite == SUITE_AES128 ? 16 : 32, shared, 32, h);
    rp_wipe(shared, sizeof shared);
    if (!rp_tls_aead_init(&t->rd, alg, t->keys.key_len, t->keys.s_hs) || !rp_tls_aead_init(&t->wr, alg, t->keys.key_len, t->keys.c_hs)) {
        return fail(t, AL_INTERNAL, "cannot set the handshake keys", why);
    }
    t->rd_on = t->wr_on = true;
    t->st = ST_WAIT_EE;
    return PROVEN_OK;
}

// ---- the server's encrypted flight --------------------------------------------------------------------

static bool ext_list_ok(const uint8_t *p, size_t n) {
    while (n) {
        if (n < 4 || be16(p + 2) > n - 4) return false;
        size_t l = be16(p + 2);
        p += 4 + l;
        n -= 4 + l;
    }
    return true;
}

static proven_err_t certificate(rp_tls_t *t, const uint8_t *b, size_t n, const char **why) {
    if (n < 4 || b[0] != 0) return fail(t, AL_DECODE, "a malformed Certificate (or one with a request context)", why);
    size_t ll = be24(b + 1);
    if (ll != n - 4) return fail(t, AL_DECODE, "a malformed Certificate", why);
    const uint8_t *p = b + 4;
    size_t left = ll, count = 0;
    t->chain.len = 0;
    while (left) {
        if (left < 3 || be24(p) == 0 || be24(p) > left - 3) return fail(t, AL_DECODE, "a malformed certificate entry", why);
        size_t cl = be24(p);
        const uint8_t *cert = p + 3;
        p += 3 + cl;
        left -= 3 + cl;
        if (left < 2 || be16(p) > left - 2 || !ext_list_ok(p + 2, be16(p))) return fail(t, AL_DECODE, "a malformed certificate entry", why);
        size_t xl = be16(p);
        p += 2 + xl;
        left -= 2 + xl;
        if (++count > MAX_CERTS) return fail(t, AL_BAD_CERT, "the server sent more than 10 certificates", why);
        if (count == 1) t->leaf_len = cl;
        rp_buf_put(&t->chain, cert, cl);
    }
    if (count == 0) return fail(t, AL_DECODE, "the server sent no certificate", why);
    if (t->chain.err != PROVEN_OK) return fail(t, AL_INTERNAL, "out of memory", why);
    rp_cert_t leaf;
    const char *w = NULL;
    if (!rp_cert_parse(t->chain.data, t->leaf_len, &leaf, &w)) return fail(t, AL_BAD_CERT, w ? w : "the server's certificate is malformed", why);
    if (!rp_cert_names_host(&leaf, t->host)) {
        static char msg[400];
        snprintf(msg, sizeof msg, "the server's certificate is not for '%s' (subjectAltName)", t->host);
        return fail(t, AL_BAD_CERT, msg, why);
    }
    rp_der_span_t signer = { t->chain.data, t->leaf_len }, rest = { t->chain.data + t->leaf_len, t->chain.len - t->leaf_len };
    if (t->anchor_count == 0) return fail(t, AL_UNKNOWN_CA, "no trust anchors for the server (--tls-trust or --system-roots)", why);
    if (!rp_chain_trusted_for(signer, rest, t->anchors, t->anchor_count, t->now, RP_PURPOSE_SERVER, &w)) {
        return fail(t, strstr(w, "expired") ? AL_EXPIRED : AL_UNKNOWN_CA, w, why);
    }
    return PROVEN_OK;
}

static proven_err_t finish(rp_tls_t *t, const uint8_t *msg, size_t len, const char **why) {
    size_t hl = t->keys.hl;
    uint8_t h[RP_HASH_MAX], want[RP_HASH_MAX];
    th(t, h);
    rp_tls_finished(t->keys.alg, t->keys.s_hs, h, want);
    if (len - 4 != hl || !rp_ct_equal(want, msg + 4, hl)) return fail(t, AL_DECRYPT, "the server's Finished does not verify", why);
    if (t->hs.len) return fail(t, AL_UNEXPECTED, "handshake data after the server's Finished in the same record", why);
    rp_buf_put(&t->tr, msg, len);
    th(t, h);
    rp_tls_keys_app(&t->keys, h);
    uint8_t fin[4 + RP_HASH_MAX] = { HS_FINISHED, 0, 0, (uint8_t)hl };
    rp_tls_finished(t->keys.alg, t->keys.c_hs, h, fin + 4);
    send_ccs(t);
    send_hs(t, fin, 4 + hl, false);
    if (!rp_tls_aead_init(&t->rd, t->keys.alg, t->keys.key_len, t->keys.s_ap) ||
        !rp_tls_aead_init(&t->wr, t->keys.alg, t->keys.key_len, t->keys.c_ap)) {
        return fail(t, AL_INTERNAL, "cannot set the application keys", why);
    }
    rp_wipe(t->keys.hs, sizeof t->keys.hs);
    rp_wipe(t->keys.c_hs, sizeof t->keys.c_hs);
    rp_wipe(t->keys.s_hs, sizeof t->keys.s_hs);
    t->st = ST_CONNECTED;
    return t->out.err == PROVEN_OK ? PROVEN_OK : fail(t, AL_INTERNAL, "out of memory", why);
}

static proven_err_t key_update(rp_tls_t *t, const uint8_t *b, size_t n, const char **why) {
    if (n != 1 || b[0] > 1) return fail(t, AL_DECODE, "a malformed KeyUpdate", why);
    if (t->hs.len) return fail(t, AL_UNEXPECTED, "handshake data after KeyUpdate in the same record", why);
    uint8_t next[RP_HASH_MAX];
    if (!rp_tls_expand_label(t->keys.alg, t->keys.s_ap, "traffic upd", NULL, 0, next, t->keys.hl)) return fail(t, AL_INTERNAL, "key update", why);
    memcpy(t->keys.s_ap, next, t->keys.hl);
    if (!rp_tls_aead_init(&t->rd, t->keys.alg, t->keys.key_len, t->keys.s_ap)) return fail(t, AL_INTERNAL, "key update", why);
    if (b[0] == 1) {                                   // update_requested: answer, then change ours
        static const uint8_t ku[5] = { HS_KEY_UPDATE, 0, 0, 1, 0 };
        send_hs(t, ku, sizeof ku, false);
        if (!rp_tls_expand_label(t->keys.alg, t->keys.c_ap, "traffic upd", NULL, 0, next, t->keys.hl)) return fail(t, AL_INTERNAL, "key update", why);
        memcpy(t->keys.c_ap, next, t->keys.hl);
        if (!rp_tls_aead_init(&t->wr, t->keys.alg, t->keys.key_len, t->keys.c_ap)) return fail(t, AL_INTERNAL, "key update", why);
    }
    rp_wipe(next, sizeof next);
    return PROVEN_OK;
}

// One whole handshake message (header included).
static proven_err_t handshake(rp_tls_t *t, const uint8_t *msg, size_t len, const char **why) {
    uint8_t type = msg[0];
    const uint8_t *b = msg + 4;
    size_t n = len - 4;
    switch (t->st) {
    case ST_WAIT_SH:
        if (type != HS_SERVER_HELLO) break;
        return server_hello(t, msg, len, why);
    case ST_WAIT_EE:
        if (type != HS_ENCRYPTED_EXTENSIONS) break;
        if (n < 2 || be16(b) != n - 2 || !ext_list_ok(b + 2, n - 2)) return fail(t, AL_DECODE, "malformed EncryptedExtensions", why);
        rp_buf_put(&t->tr, msg, len);
        t->st = ST_WAIT_CERT;
        return PROVEN_OK;
    case ST_WAIT_CERT:
        if (type == HS_CERTIFICATE_REQUEST) {
            return fail(t, AL_HANDSHAKE, "the server asks for a client certificate, which rubrapack does not send", why);
        }
        if (type != HS_CERTIFICATE) break;
        {
            proven_err_t err = certificate(t, b, n, why);
            if (err != PROVEN_OK) return err;
        }
        rp_buf_put(&t->tr, msg, len);
        t->st = ST_WAIT_CV;
        return PROVEN_OK;
    case ST_WAIT_CV: {
        if (type != HS_CERTIFICATE_VERIFY) break;
        if (n < 4 || be16(b + 2) != n - 4) return fail(t, AL_DECODE, "a malformed CertificateVerify", why);
        uint8_t h[RP_HASH_MAX];
        th(t, h);
        const char *w = NULL;
        if (!rp_tls_cv_check(t->chain.data, t->leaf_len, be16(b), b + 4, n - 4, h, t->keys.hl, &w)) return fail(t, AL_DECRYPT, w, why);
        rp_buf_put(&t->tr, msg, len);
        t->st = ST_WAIT_FIN;
        return PROVEN_OK;
    }
    case ST_WAIT_FIN:
        if (type != HS_FINISHED) break;
        return finish(t, msg, len, why);
    case ST_CONNECTED:
        if (type == HS_NEW_SESSION_TICKET) return PROVEN_OK;       // no resumption: ignored
        if (type == HS_KEY_UPDATE) return key_update(t, b, n, why);
        break;
    case ST_FAILED:
        return PROVEN_ERR_INVALID_STATE;
    }
    return fail(t, AL_UNEXPECTED, "the server sent a handshake message out of order", why);
}

static proven_err_t content(rp_tls_t *t, uint8_t type, const uint8_t *p, size_t n, const char **why) {
    if (type == CT_ALERT) {
        if (n != 2) return fail(t, AL_DECODE, "a malformed alert", why);
        if (p[1] == AL_CLOSE) {
            t->eof = true;
            return PROVEN_OK;
        }
        static char msg[200];
        const char *name = p[1] == AL_HANDSHAKE ? "handshake_failure" : p[1] == AL_PROTOCOL_VERSION ? "protocol_version"
                         : p[1] == AL_BAD_CERT ? "bad_certificate" : p[1] == AL_DECODE ? "decode_error" : p[1] == AL_ILLEGAL ? "illegal_parameter"
                         : p[1] == AL_INTERNAL ? "internal_error" : p[1] == 112 ? "unrecognized_name" : p[1] == 71 ? "insufficient_security" : "";
        bool early = t->st == ST_WAIT_SH && (p[1] == AL_HANDSHAKE || p[1] == AL_PROTOCOL_VERSION || p[1] == 71);
        snprintf(msg, sizeof msg, "the server ended the handshake with TLS alert %u%s%s%s%s", (unsigned)p[1], *name ? " (" : "", name,
                 *name ? ")" : "", early ? " - it may not speak TLS 1.3, which rubrapack needs" : "");
        *why = msg;
        t->st = ST_FAILED;
        return PROVEN_ERR_IO;
    }
    if (type == CT_APP) {
        if (t->st != ST_CONNECTED) return fail(t, AL_UNEXPECTED, "application data before the handshake finished", why);
        rp_buf_put(&t->app, p, n);
        return t->app.err == PROVEN_OK ? PROVEN_OK : fail(t, AL_INTERNAL, "more data than rubrapack keeps", why);
    }
    if (type != CT_HANDSHAKE || n == 0) return fail(t, AL_UNEXPECTED, "an unexpected record", why);
    rp_buf_put(&t->hs, p, n);
    if (t->hs.err != PROVEN_OK) return fail(t, AL_OVERFLOW, "a handshake message larger than 64 KiB", why);
    size_t pos = 0;
    while (t->hs.len - pos >= 4) {
        size_t ml = be24(t->hs.data + pos + 1);
        if (ml > MAX_HS) return fail(t, AL_OVERFLOW, "a handshake message larger than 64 KiB", why);
        if (t->hs.len - pos < 4 + ml) break;
        // Take the message out first: the checks for leftovers look at what remains.
        uint8_t *msg = rp_mem_alloc(t->alloc, 4 + ml, 1);
        if (msg == NULL) return fail(t, AL_INTERNAL, "out of memory", why);
        memcpy(msg, t->hs.data + pos, 4 + ml);
        pos += 4 + ml;
        memmove(t->hs.data, t->hs.data + pos, t->hs.len - pos);
        t->hs.len -= pos;
        pos = 0;
        proven_err_t err = handshake(t, msg, 4 + ml, why);
        rp_mem_free(t->alloc, msg);
        if (err != PROVEN_OK) return err;
    }
    return PROVEN_OK;
}

proven_err_t rp_tls_feed(rp_tls_t *t, const uint8_t *in, size_t len, const char **why) {
    if (t->st == ST_FAILED) {
        *why = "the connection has failed";
        return PROVEN_ERR_INVALID_STATE;
    }
    rp_buf_put(&t->in, in, len);
    if (t->in.err != PROVEN_OK) return fail(t, AL_INTERNAL, "out of memory", why);
    size_t pos = 0;
    proven_err_t err = PROVEN_OK;
    while (err == PROVEN_OK && t->in.len - pos >= 5 && !t->eof) {
        const uint8_t *r = t->in.data + pos;
        size_t rl = be16(r + 3);
        if (rl > (t->rd_on ? RP_TLS_MAX_CIPHER : RP_TLS_MAX_PLAIN)) {
            err = fail(t, AL_OVERFLOW, "a record longer than TLS allows", why);
            break;
        }
        if (t->in.len - pos < 5 + rl) break;
        if (r[0] == CT_CCS && t->st != ST_CONNECTED && (t->st != ST_WAIT_SH || t->hrr)) {
            // Middlebox compatibility: one byte 0x01, not encrypted, ignored (RFC 8446 5).
            if (rl != 1 || r[5] != 1) err = fail(t, AL_UNEXPECTED, "a malformed change_cipher_spec", why);
        } else if (t->rd_on) {
            if (r[0] != CT_APP) {
                err = fail(t, AL_UNEXPECTED, "an unprotected record after ServerHello", why);
            } else {
                uint8_t type;
                size_t n;
                if (!rp_tls_open(&t->rd, r, 5 + rl, t->rec, &n, &type)) err = fail(t, AL_BAD_MAC, "a record that does not decrypt", why);
                else err = content(t, type, t->rec, n, why);
            }
        } else if (r[0] == CT_HANDSHAKE || r[0] == CT_ALERT) {
            err = content(t, r[0], r + 5, rl, why);
        } else {
            err = fail(t, AL_UNEXPECTED, "an unexpected record type", why);
        }
        pos += 5 + rl;
    }
    memmove(t->in.data, t->in.data + pos, t->in.len - pos);
    t->in.len -= pos;
    return err;
}

proven_err_t rp_tls_write(rp_tls_t *t, const uint8_t *data, size_t len) {
    if (t->st != ST_CONNECTED) return PROVEN_ERR_INVALID_STATE;
    while (len) {
        size_t take = len < RP_TLS_MAX_PLAIN ? len : RP_TLS_MAX_PLAIN, n;
        rp_tls_seal(&t->wr, CT_APP, data, take, t->rec, &n);
        rp_buf_put(&t->out, t->rec, n);
        data += take;
        len -= take;
    }
    return t->out.err;
}

size_t rp_tls_read(rp_tls_t *t, uint8_t *buf, size_t cap) {
    size_t n = t->app.len - t->app_pos;
    if (n > cap) n = cap;
    if (n == 0) return 0;                               // (app.data may still be NULL)
    memcpy(buf, t->app.data + t->app_pos, n);
    t->app_pos += n;
    if (t->app_pos == t->app.len) t->app_pos = t->app.len = 0;
    return n;
}

void rp_tls_close(rp_tls_t *t) {
    if (t->st != ST_CONNECTED) return;
    static const uint8_t a[2] = { 1, AL_CLOSE };
    size_t n;
    rp_tls_seal(&t->wr, CT_ALERT, a, 2, t->rec, &n);
    rp_buf_put(&t->out, t->rec, n);
}
