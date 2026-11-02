#include "proven_internal_tls.h"
#include "../../platform/proven_sys_mem.h"

/* TLS 1.2 below the state machine (RFC 5246): the suites, the PRF and what is derived with
 * it, and record protection for the AEAD suites (RFC 5288, RFC 7905). */

static const proven_tls12_suite_t TLS12_SUITES[] = {
    { 0xc02b, PROVEN_HMAC_SHA256, 16, false, false, 0, PROVEN_TLS_KX_ECDHE, 0 },      /* ECDHE-ECDSA-AES128-GCM-SHA256 */
    { 0xc02c, PROVEN_HMAC_SHA384, 32, false, false, 0, PROVEN_TLS_KX_ECDHE, 0 },      /* ECDHE-ECDSA-AES256-GCM-SHA384 */
    { 0xcca9, PROVEN_HMAC_SHA256, 32, true,  false, 0, PROVEN_TLS_KX_ECDHE, 0 },      /* ECDHE-ECDSA-CHACHA20-POLY1305 */
    { 0xc02f, PROVEN_HMAC_SHA256, 16, false, true,  0, PROVEN_TLS_KX_ECDHE, 0 },      /* ECDHE-RSA-AES128-GCM-SHA256 */
    { 0xc030, PROVEN_HMAC_SHA384, 32, false, true,  0, PROVEN_TLS_KX_ECDHE, 0 },      /* ECDHE-RSA-AES256-GCM-SHA384 */
    { 0xcca8, PROVEN_HMAC_SHA256, 32, true,  true,  0, PROVEN_TLS_KX_ECDHE, 0 },      /* ECDHE-RSA-CHACHA20-POLY1305 */
    /* The legacy set, in the order it is offered and chosen: always after everything above.
     * `hash` is the PRF's from TLS 1.2 on; the record MAC is `mac`. */
    { 0x009e, PROVEN_HMAC_SHA256, 16, false, true,  0,                    PROVEN_TLS_KX_DHE,   4 },   /* DHE-RSA-AES128-GCM-SHA256 */
    { 0x009f, PROVEN_HMAC_SHA384, 32, false, true,  0,                    PROVEN_TLS_KX_DHE,   4 },   /* DHE-RSA-AES256-GCM-SHA384 */
    { 0xccaa, PROVEN_HMAC_SHA256, 32, true,  true,  0,                    PROVEN_TLS_KX_DHE,   4 },   /* DHE-RSA-CHACHA20-POLY1305 */
    { 0xc023, PROVEN_HMAC_SHA256, 16, false, false, PROVEN_TLS_LH_SHA256, PROVEN_TLS_KX_ECDHE, 1 },   /* ECDHE-ECDSA-AES128-SHA256 */
    { 0xc027, PROVEN_HMAC_SHA256, 16, false, true,  PROVEN_TLS_LH_SHA256, PROVEN_TLS_KX_ECDHE, 1 },   /* ECDHE-RSA-AES128-SHA256 */
    { 0xc024, PROVEN_HMAC_SHA384, 32, false, false, PROVEN_TLS_LH_SHA384, PROVEN_TLS_KX_ECDHE, 1 },   /* ECDHE-ECDSA-AES256-SHA384 */
    { 0xc028, PROVEN_HMAC_SHA384, 32, false, true,  PROVEN_TLS_LH_SHA384, PROVEN_TLS_KX_ECDHE, 1 },   /* ECDHE-RSA-AES256-SHA384 */
    { 0xc009, PROVEN_HMAC_SHA256, 16, false, false, PROVEN_TLS_LH_SHA1,   PROVEN_TLS_KX_ECDHE, 1 },   /* ECDHE-ECDSA-AES128-SHA */
    { 0xc013, PROVEN_HMAC_SHA256, 16, false, true,  PROVEN_TLS_LH_SHA1,   PROVEN_TLS_KX_ECDHE, 1 },   /* ECDHE-RSA-AES128-SHA */
    { 0xc00a, PROVEN_HMAC_SHA256, 32, false, false, PROVEN_TLS_LH_SHA1,   PROVEN_TLS_KX_ECDHE, 1 },   /* ECDHE-ECDSA-AES256-SHA */
    { 0xc014, PROVEN_HMAC_SHA256, 32, false, true,  PROVEN_TLS_LH_SHA1,   PROVEN_TLS_KX_ECDHE, 1 },   /* ECDHE-RSA-AES256-SHA */
    { 0x0067, PROVEN_HMAC_SHA256, 16, false, true,  PROVEN_TLS_LH_SHA256, PROVEN_TLS_KX_DHE,   5 },   /* DHE-RSA-AES128-SHA256 */
    { 0x006b, PROVEN_HMAC_SHA256, 32, false, true,  PROVEN_TLS_LH_SHA256, PROVEN_TLS_KX_DHE,   5 },   /* DHE-RSA-AES256-SHA256 */
    { 0x0033, PROVEN_HMAC_SHA256, 16, false, true,  PROVEN_TLS_LH_SHA1,   PROVEN_TLS_KX_DHE,   5 },   /* DHE-RSA-AES128-SHA */
    { 0x0039, PROVEN_HMAC_SHA256, 32, false, true,  PROVEN_TLS_LH_SHA1,   PROVEN_TLS_KX_DHE,   5 },   /* DHE-RSA-AES256-SHA */
    { 0x009c, PROVEN_HMAC_SHA256, 16, false, true,  0,                    PROVEN_TLS_KX_RSA,   2 },   /* AES128-GCM-SHA256 */
    { 0x009d, PROVEN_HMAC_SHA384, 32, false, true,  0,                    PROVEN_TLS_KX_RSA,   2 },   /* AES256-GCM-SHA384 */
    { 0x003c, PROVEN_HMAC_SHA256, 16, false, true,  PROVEN_TLS_LH_SHA256, PROVEN_TLS_KX_RSA,   3 },   /* AES128-SHA256 */
    { 0x003d, PROVEN_HMAC_SHA256, 32, false, true,  PROVEN_TLS_LH_SHA256, PROVEN_TLS_KX_RSA,   3 },   /* AES256-SHA256 */
    { 0x002f, PROVEN_HMAC_SHA256, 16, false, true,  PROVEN_TLS_LH_SHA1,   PROVEN_TLS_KX_RSA,   3 },   /* AES128-SHA */
    { 0x0035, PROVEN_HMAC_SHA256, 32, false, true,  PROVEN_TLS_LH_SHA1,   PROVEN_TLS_KX_RSA,   3 },   /* AES256-SHA */
};

const proven_tls12_suite_t *proven_tls12_suite_at(proven_size_t index) {
    return index < sizeof TLS12_SUITES / sizeof TLS12_SUITES[0] ? &TLS12_SUITES[index] : NULL;
}

const proven_tls12_suite_t *proven_tls12_suite_find(proven_u16 id) {
    for (proven_size_t i = 0; i < sizeof TLS12_SUITES / sizeof TLS12_SUITES[0]; ++i) {
        if (TLS12_SUITES[i].id == id) return &TLS12_SUITES[i];
    }
    return (void *)0;
}

static proven_size_t label_len(const char *s) { proven_size_t n = 0; while (s[n]) ++n; return n; }

/* P_hash(secret, label | seed): A(0) = label | seed, A(i) = HMAC(secret, A(i-1)), and the
 * output is HMAC(secret, A(1) | label | seed) | HMAC(secret, A(2) | label | seed) | ... */
void proven_tls12_prf(proven_hmac_hash_t hash, proven_mem_view_t secret, const char *label, proven_mem_view_t seed,
                      proven_byte_t *out, proven_size_t out_len) {
    const proven_size_t hlen = proven_hmac_size(hash), llen = label_len(label);
    proven_byte_t a[PROVEN_HMAC_MAX_SIZE], block[PROVEN_HMAC_MAX_SIZE];
    proven_hmac_t h;
    proven_mem_view_t lv = { .ptr = (const proven_byte_t *)label, .size = llen };
    (void)proven_hmac_init(&h, hash, secret);
    proven_hmac_update(&h, lv);
    proven_hmac_update(&h, seed);
    proven_hmac_final(&h, a);
    for (proven_size_t done = 0; done < out_len;) {
        (void)proven_hmac_init(&h, hash, secret);
        proven_hmac_update(&h, (proven_mem_view_t){ .ptr = a, .size = hlen });
        proven_hmac_update(&h, lv);
        proven_hmac_update(&h, seed);
        proven_hmac_final(&h, block);
        for (proven_size_t i = 0; i < hlen && done < out_len; ++i, ++done) out[done] = block[i];
        (void)proven_hmac_init(&h, hash, secret);
        proven_hmac_update(&h, (proven_mem_view_t){ .ptr = a, .size = hlen });
        proven_hmac_final(&h, a);
    }
    proven_mem_wipe((proven_mem_mut_t){ .ptr = a, .size = sizeof a });
    proven_mem_wipe((proven_mem_mut_t){ .ptr = block, .size = sizeof block });
    proven_mem_wipe((proven_mem_mut_t){ .ptr = (proven_byte_t *)&h, .size = sizeof h });
}

void proven_tls12_master_secret(proven_hmac_hash_t hash, proven_mem_view_t premaster, proven_mem_view_t session_hash,
                                proven_byte_t master[PROVEN_TLS12_MASTER_SIZE]) {
    /* RFC 7627: the session hash - the handshake through ClientKeyExchange - takes the place of
     * the two randoms, so that the master secret is bound to this handshake and no other. */
    proven_tls12_prf(hash, premaster, "extended master secret", session_hash, master, PROVEN_TLS12_MASTER_SIZE);
}

void proven_tls12_finished(proven_hmac_hash_t hash, const proven_byte_t master[PROVEN_TLS12_MASTER_SIZE], bool server,
                           proven_mem_view_t handshake_hash, proven_byte_t out[PROVEN_TLS12_VERIFY_SIZE]) {
    proven_tls12_prf(hash, (proven_mem_view_t){ .ptr = master, .size = PROVEN_TLS12_MASTER_SIZE },
                     server ? "server finished" : "client finished", handshake_hash, out, PROVEN_TLS12_VERIFY_SIZE);
}

static void tls12_install(proven_tls_keys_t *keys, const proven_tls12_suite_t *suite, const proven_byte_t *key, const proven_byte_t *iv) {
    const proven_size_t iv_len = suite->chacha ? 12 : 4;
    for (proven_size_t i = 0; i < 12; ++i) keys->iv[i] = i < iv_len ? iv[i] : 0;
    keys->chacha = suite->chacha;
    if (suite->chacha) {
        for (int i = 0; i < 32; ++i) keys->aes.rk[i] = key[i];
    } else {
        (void)proven_crypto_aes_gcm_init(&keys->aes, key, suite->key_len);       /* 16 or 32: cannot fail */
    }
    keys->seq = 0;
    keys->active = true;
}

void proven_tls12_set_keys(proven_tls_keys_t *client_write, proven_tls_keys_t *server_write, const proven_tls12_suite_t *suite,
                           const proven_byte_t master[PROVEN_TLS12_MASTER_SIZE], const proven_byte_t client_random[32],
                           const proven_byte_t server_random[32]) {
    /* key_block = PRF(master, "key expansion", server_random | client_random): the two keys,
     * then the two implicit IVs. The AEAD suites have no MAC keys. */
    const proven_size_t iv_len = suite->chacha ? 12 : 4, need = 2 * suite->key_len + 2 * iv_len;
    proven_byte_t seed[64], block[2 * 32 + 2 * 12];
    for (int i = 0; i < 32; ++i) { seed[i] = server_random[i]; seed[32 + i] = client_random[i]; }
    proven_tls12_prf(suite->hash, (proven_mem_view_t){ .ptr = master, .size = PROVEN_TLS12_MASTER_SIZE }, "key expansion",
                     (proven_mem_view_t){ .ptr = seed, .size = sizeof seed }, block, need);
    tls12_install(client_write, suite, block, block + 2 * suite->key_len);
    tls12_install(server_write, suite, block + suite->key_len, block + 2 * suite->key_len + iv_len);
    proven_mem_wipe((proven_mem_mut_t){ .ptr = block, .size = sizeof block });
}

/* What is authenticated beside the content: seq | type | version | length of the plaintext. */
static void tls12_aad(const proven_tls_keys_t *keys, proven_byte_t type, proven_size_t plain_len, proven_byte_t aad[13]) {
    for (int i = 0; i < 8; ++i) aad[i] = (proven_byte_t)(keys->seq >> (8 * (7 - i)));
    aad[8] = type; aad[9] = 0x03; aad[10] = 0x03;
    aad[11] = (proven_byte_t)(plain_len >> 8); aad[12] = (proven_byte_t)plain_len;
}

/* GCM: the four implicit bytes, then eight that travel with the record - the sequence number,
 * so that a nonce can never repeat under one key. ChaCha20: the IV with the sequence number
 * XORed into its last eight bytes, and nothing on the wire. */
static void tls12_nonce(const proven_tls_keys_t *keys, proven_byte_t nonce[12]) {
    if (keys->chacha) {
        for (int i = 0; i < 12; ++i) nonce[i] = keys->iv[i];
        for (int i = 0; i < 8; ++i) nonce[11 - i] ^= (proven_byte_t)(keys->seq >> (8 * i));
    } else {
        for (int i = 0; i < 4; ++i) nonce[i] = keys->iv[i];
        for (int i = 0; i < 8; ++i) nonce[4 + i] = (proven_byte_t)(keys->seq >> (8 * (7 - i)));
    }
}

proven_size_t proven_tls12_seal(proven_tls_keys_t *keys, proven_byte_t type, proven_mem_view_t content, proven_byte_t *out) {
    const proven_size_t explicit_len = keys->chacha ? 0 : 8, body = explicit_len + content.size + 16;
    proven_byte_t nonce[12], aad[13];
    out[0] = type; out[1] = 0x03; out[2] = 0x03;
    out[3] = (proven_byte_t)(body >> 8); out[4] = (proven_byte_t)body;
    proven_byte_t *p = out + PROVEN_TLS_RECORD_HEADER;
    tls12_nonce(keys, nonce);
    tls12_aad(keys, type, content.size, aad);
    for (proven_size_t i = 0; i < explicit_len; ++i) p[i] = nonce[4 + i];
    p += explicit_len;
    proven_sys_mem_copy(p, content.ptr, content.size);
    proven_mem_view_t ad = { .ptr = aad, .size = sizeof aad }, plain = { .ptr = p, .size = content.size };
    if (keys->chacha) proven_crypto_chacha20poly1305_seal(keys->aes.rk, nonce, ad, plain, p, p + content.size);
    else proven_crypto_aes_gcm_seal(&keys->aes, nonce, ad, plain, p, p + content.size);
    keys->seq++;
    return PROVEN_TLS_RECORD_HEADER + body;
}

bool proven_tls12_open(proven_tls_keys_t *keys, proven_byte_t *record, proven_size_t body_len, proven_mem_mut_t *content) {
    const proven_size_t explicit_len = keys->chacha ? 0 : 8;
    if (body_len < explicit_len + 16 || body_len > PROVEN_TLS_MAX_CIPHERTEXT) return false;
    const proven_size_t plain_len = body_len - explicit_len - 16;
    if (plain_len > PROVEN_TLS_MAX_PLAINTEXT) return false;
    proven_byte_t nonce[12], aad[13];
    proven_byte_t *p = record + PROVEN_TLS_RECORD_HEADER;
    tls12_nonce(keys, nonce);
    /* The eight bytes on the wire are the peer's choice; they are used as sent. (This side
     * sends its sequence number, as RFC 5288 recommends; a peer may send anything that never repeats.) */
    for (proven_size_t i = 0; i < explicit_len; ++i) nonce[4 + i] = p[i];
    tls12_aad(keys, record[0], plain_len, aad);
    p += explicit_len;
    proven_mem_view_t ad = { .ptr = aad, .size = sizeof aad }, cipher = { .ptr = p, .size = plain_len };
    bool ok = keys->chacha ? proven_crypto_chacha20poly1305_open(keys->aes.rk, nonce, ad, cipher, p + plain_len, p)
                           : proven_crypto_aes_gcm_open(&keys->aes, nonce, ad, cipher, p + plain_len, p);
    if (!ok) return false;
    keys->seq++;
    PROVEN_CT_PUBLIC(p, plain_len);               /* content for the layer above, as in 1.3 */
    content->ptr = p;
    content->size = plain_len;
    return true;
}
