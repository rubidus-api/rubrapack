#include "proven_internal_tls.h"
#include "../../platform/proven_sys_mem.h"

/* TLS 1.3 below the state machine: suites, transcript, key schedule, record protection. */

static const proven_tls_suite_t TLS_SUITES[] = {
    { PROVEN_TLS_AES_128_GCM_SHA256, PROVEN_HMAC_SHA256, 32, 16, false },
    { PROVEN_TLS_AES_256_GCM_SHA384, PROVEN_HMAC_SHA384, 48, 32, false },
    { PROVEN_TLS_CHACHA20_POLY1305_SHA256, PROVEN_HMAC_SHA256, 32, 32, true },
};

const proven_tls_suite_t *proven_tls_suite_find(proven_u16 id) {
    for (proven_size_t i = 0; i < sizeof TLS_SUITES / sizeof TLS_SUITES[0]; ++i) {
        if (TLS_SUITES[i].id == id) return &TLS_SUITES[i];
    }
    return NULL;
}

void proven_tls_transcript_init(proven_tls_transcript_t *t, proven_hmac_hash_t hash) {
    t->hash = hash;
    if (hash == PROVEN_HMAC_SHA256) proven_sha256_init(&t->ctx.s256);
    else proven_sha384_init(&t->ctx.s512);
}

void proven_tls_transcript_update(proven_tls_transcript_t *t, proven_mem_view_t data) {
    if (data.size == 0) return;
    if (t->hash == PROVEN_HMAC_SHA256) proven_sha256_update(&t->ctx.s256, data);
    else proven_sha512_update(&t->ctx.s512, data);
}

proven_size_t proven_tls_transcript_hash(const proven_tls_transcript_t *t, proven_byte_t out[PROVEN_HMAC_MAX_SIZE]) {
    /* The context is a plain value: finishing a copy leaves the original running. */
    if (t->hash == PROVEN_HMAC_SHA256) {
        proven_sha256_t copy = t->ctx.s256;
        proven_sha256_final(&copy, out);
        return 32;
    }
    proven_sha512_t copy = t->ctx.s512;
    proven_sha384_final(&copy, out);
    return 48;
}

void proven_tls_transcript_retry(proven_tls_transcript_t *t) {
    proven_byte_t msg[4 + PROVEN_HMAC_MAX_SIZE];
    proven_size_t n = proven_tls_transcript_hash(t, msg + 4);
    msg[0] = 254;                                 /* message_hash */
    msg[1] = 0; msg[2] = 0; msg[3] = (proven_byte_t)n;
    proven_tls_transcript_init(t, t->hash);
    proven_tls_transcript_update(t, (proven_mem_view_t){ .ptr = msg, .size = 4 + n });
}

void proven_tls13_expand_label(proven_hmac_hash_t hash, const proven_byte_t *secret, const char *label,
                               proven_mem_view_t context, proven_byte_t *out, proven_size_t out_len) {
    /* struct { uint16 length; opaque label<7..255> = "tls13 " + label; opaque context<0..255>; } */
    proven_byte_t info[2 + 1 + 6 + 32 + 1 + PROVEN_HMAC_MAX_SIZE];
    proven_size_t n = 0, label_len = 0;
    while (label[label_len] && label_len < 32) label_len++;
    proven_size_t ctx_len = context.size <= PROVEN_HMAC_MAX_SIZE ? context.size : PROVEN_HMAC_MAX_SIZE;
    info[n++] = (proven_byte_t)(out_len >> 8); info[n++] = (proven_byte_t)out_len;
    info[n++] = (proven_byte_t)(6 + label_len);
    static const char prefix[6] = { 't', 'l', 's', '1', '3', ' ' };
    for (int i = 0; i < 6; ++i) info[n++] = (proven_byte_t)prefix[i];
    for (proven_size_t i = 0; i < label_len; ++i) info[n++] = (proven_byte_t)label[i];
    info[n++] = (proven_byte_t)ctx_len;
    for (proven_size_t i = 0; i < ctx_len; ++i) info[n++] = context.ptr[i];
    /* The lengths are fixed by the callers in this unit, so the expansion cannot fail. */
    (void)proven_hkdf_expand(hash, (proven_mem_view_t){ .ptr = secret, .size = proven_hmac_size(hash) },
                             (proven_mem_view_t){ .ptr = info, .size = n }, (proven_mem_mut_t){ .ptr = out, .size = out_len });
}

void proven_tls13_derive_secret(proven_hmac_hash_t hash, const proven_byte_t *secret, const char *label,
                                const proven_byte_t *transcript_hash, proven_byte_t *out) {
    proven_size_t n = proven_hmac_size(hash);
    proven_tls13_expand_label(hash, secret, label, (proven_mem_view_t){ .ptr = transcript_hash, .size = n }, out, n);
}

void proven_tls13_extract(proven_hmac_hash_t hash, const proven_byte_t *salt, proven_mem_view_t ikm, proven_byte_t *out) {
    static const proven_byte_t zeros[PROVEN_HMAC_MAX_SIZE] = { 0 };
    proven_size_t n = proven_hmac_size(hash);
    proven_byte_t prk[PROVEN_HMAC_MAX_SIZE];
    if (!ikm.ptr) { ikm.ptr = zeros; ikm.size = n; }
    (void)proven_hkdf_extract(hash, (proven_mem_view_t){ .ptr = salt ? salt : zeros, .size = n }, ikm, prk);
    for (proven_size_t i = 0; i < n; ++i) out[i] = prk[i];
    proven_mem_wipe((proven_mem_mut_t){ .ptr = prk, .size = sizeof prk });
}

void proven_tls13_finished(proven_hmac_hash_t hash, const proven_byte_t *base_secret, const proven_byte_t *transcript_hash,
                           proven_byte_t *out) {
    proven_size_t n = proven_hmac_size(hash);
    proven_byte_t key[PROVEN_HMAC_MAX_SIZE], mac[PROVEN_HMAC_MAX_SIZE];
    proven_tls13_expand_label(hash, base_secret, "finished", (proven_mem_view_t){ .ptr = key, .size = 0 }, key, n);
    (void)proven_hmac(hash, (proven_mem_view_t){ .ptr = key, .size = n }, (proven_mem_view_t){ .ptr = transcript_hash, .size = n }, mac);
    for (proven_size_t i = 0; i < n; ++i) out[i] = mac[i];
    proven_mem_wipe((proven_mem_mut_t){ .ptr = key, .size = sizeof key });
    proven_mem_wipe((proven_mem_mut_t){ .ptr = mac, .size = sizeof mac });
}

void proven_tls13_set_keys(proven_tls_keys_t *keys, const proven_tls_suite_t *suite, const proven_byte_t *traffic_secret) {
    proven_byte_t key[32];
    proven_mem_view_t none = { .ptr = key, .size = 0 };
    proven_tls13_expand_label(suite->hash, traffic_secret, "key", none, key, suite->key_len);
    proven_tls13_expand_label(suite->hash, traffic_secret, "iv", none, keys->iv, 12);
    keys->chacha = suite->chacha;
    if (suite->chacha) {
        for (int i = 0; i < 32; ++i) keys->aes.rk[i] = key[i];
    } else {
        (void)proven_crypto_aes_gcm_init(&keys->aes, key, suite->key_len);       /* 16 or 32: cannot fail */
    }
    keys->seq = 0;
    keys->active = true;
    proven_mem_wipe((proven_mem_mut_t){ .ptr = key, .size = sizeof key });
}

/* The per-record nonce: the IV with the sequence number XORed into its last eight bytes. */
static void tls13_nonce(const proven_tls_keys_t *keys, proven_byte_t nonce[12]) {
    for (int i = 0; i < 12; ++i) nonce[i] = keys->iv[i];
    for (int i = 0; i < 8; ++i) nonce[11 - i] ^= (proven_byte_t)(keys->seq >> (8 * i));
}

proven_size_t proven_tls13_seal(proven_tls_keys_t *keys, proven_byte_t type, proven_mem_view_t content, proven_byte_t *out) {
    proven_size_t inner = content.size + 1, body = inner + 16;
    proven_byte_t nonce[12];
    out[0] = PROVEN_TLS_CT_APPLICATION; out[1] = 0x03; out[2] = 0x03;
    out[3] = (proven_byte_t)(body >> 8); out[4] = (proven_byte_t)body;
    proven_byte_t *p = out + PROVEN_TLS_RECORD_HEADER;
    proven_sys_mem_copy(p, content.ptr, content.size);
    p[content.size] = type;
    tls13_nonce(keys, nonce);
    proven_mem_view_t aad = { .ptr = out, .size = PROVEN_TLS_RECORD_HEADER };
    proven_mem_view_t plain = { .ptr = p, .size = inner };
    if (keys->chacha) proven_crypto_chacha20poly1305_seal(keys->aes.rk, nonce, aad, plain, p, p + inner);
    else proven_crypto_aes_gcm_seal(&keys->aes, nonce, aad, plain, p, p + inner);
    keys->seq++;
    return PROVEN_TLS_RECORD_HEADER + body;
}

bool proven_tls13_open(proven_tls_keys_t *keys, proven_byte_t *record, proven_size_t body_len,
                       proven_byte_t *type, proven_mem_mut_t *content) {
    if (body_len < 17 || body_len > PROVEN_TLS_MAX_CIPHERTEXT) return false;
    proven_byte_t nonce[12];
    proven_byte_t *p = record + PROVEN_TLS_RECORD_HEADER;
    proven_size_t inner = body_len - 16;
    tls13_nonce(keys, nonce);
    proven_mem_view_t aad = { .ptr = record, .size = PROVEN_TLS_RECORD_HEADER };
    proven_mem_view_t cipher = { .ptr = p, .size = inner };
    bool ok = keys->chacha ? proven_crypto_chacha20poly1305_open(keys->aes.rk, nonce, aad, cipher, p + inner, p)
                           : proven_crypto_aes_gcm_open(&keys->aes, nonce, aad, cipher, p + inner, p);
    if (!ok) return false;
    keys->seq++;
    /* From here the record's content is data for the layer above, which reads and branches on
     * it as any parser does. What stays secret is the keys. */
    PROVEN_CT_PUBLIC(p, inner);
    /* The content type is the last byte that is not zero; a record of nothing but zeros has none. */
    while (inner > 0 && p[inner - 1] == 0) inner--;
    if (inner == 0) return false;
    *type = p[inner - 1];
    content->ptr = p;
    content->size = inner - 1;
    return content->size <= PROVEN_TLS_MAX_PLAINTEXT;
}
