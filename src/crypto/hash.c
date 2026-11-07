// src/crypto/hash.c - SHA-256/384/512, SHA-1, HMAC and PBKDF2 behind one interface
// (include/rubrapack/crypto.h), all proven's.

#include "rubrapack/crypto.h"

#include <string.h>

static proven_mem_view_t mv(const void *p, size_t n) { return (proven_mem_view_t){ (const proven_byte_t *)p, n }; }

size_t rp_hash_size(rp_hash_alg_t alg) { return alg == RP_HASH_SHA256 ? 32 : alg == RP_HASH_SHA384 ? 48 : 64; }
size_t rp_hash_block(rp_hash_alg_t alg) { return alg == RP_HASH_SHA256 ? 64 : 128; }

void rp_hash_init(rp_hash_t *h, rp_hash_alg_t alg) {
    h->alg = alg;
    if (alg == RP_HASH_SHA256) proven_sha256_init(&h->u.s256);
    else if (alg == RP_HASH_SHA384) proven_sha384_init(&h->u.s512);
    else proven_sha512_init(&h->u.s512);
}

void rp_hash_update(rp_hash_t *h, const void *data, size_t len) {
    if (h->alg == RP_HASH_SHA256) proven_sha256_update(&h->u.s256, mv(data, len));
    else if (h->alg == RP_HASH_SHA384) proven_sha384_update(&h->u.s512, mv(data, len));
    else proven_sha512_update(&h->u.s512, mv(data, len));
}

void rp_hash_final(rp_hash_t *h, uint8_t *out) {
    if (h->alg == RP_HASH_SHA256) proven_sha256_final(&h->u.s256, out);
    else if (h->alg == RP_HASH_SHA384) proven_sha384_final(&h->u.s512, out);
    else proven_sha512_final(&h->u.s512, out);
    rp_wipe(h, sizeof *h);
}

void rp_hash(rp_hash_alg_t alg, const void *data, size_t len, uint8_t *out) {
    rp_hash_t h;
    rp_hash_init(&h, alg);
    rp_hash_update(&h, data, len);
    rp_hash_final(&h, out);
}

void rp_wipe(void *p, size_t n) { proven_mem_wipe((proven_mem_mut_t){ p, n }); }

bool rp_ct_equal(const uint8_t *a, const uint8_t *b, size_t n) { return proven_mem_equal_ct(mv(a, n), mv(b, n)); }

// ---- HMAC and PBKDF2 --------------------------------------------------------------------------

void rp_hmac_init(rp_hmac_t *m, rp_hash_alg_t alg, const uint8_t *key, size_t key_len) {
    // Fails only for an unknown hash or a NULL key with a length, neither of which reaches here.
    (void)proven_hmac_init(&m->m, rp_hash_mac(alg), mv(key_len ? key : (const uint8_t *)"", key_len));
}

void rp_hmac_update(rp_hmac_t *m, const void *data, size_t len) {
    if (len) proven_hmac_update(&m->m, mv(data, len));
}

void rp_hmac_final(rp_hmac_t *m, uint8_t *out) {
    uint8_t d[PROVEN_HMAC_MAX_SIZE];
    size_t n = proven_hmac_size(m->m.hash);
    proven_hmac_final(&m->m, d);
    memcpy(out, d, n);
    rp_wipe(d, sizeof d);
}

void rp_hmac(rp_hash_alg_t alg, const uint8_t *key, size_t key_len, const void *data, size_t len, uint8_t *out) {
    rp_hmac_t m;
    rp_hmac_init(&m, alg, key, key_len);
    rp_hmac_update(&m, data, len);
    rp_hmac_final(&m, out);
}

void rp_pbkdf2(rp_hash_alg_t alg, const uint8_t *pass, size_t pass_len, const uint8_t *salt, size_t salt_len,
               uint32_t iterations, uint8_t *out, size_t out_len) {
    // Fails only for zero iterations or a NULL buffer with a length, neither of which reaches here.
    (void)proven_pbkdf2(rp_hash_mac(alg), mv(pass_len ? pass : (const uint8_t *)"", pass_len),
                        mv(salt_len ? salt : (const uint8_t *)"", salt_len), iterations, (proven_mem_mut_t){ out, out_len });
}

// ---- SHA-1, for catalog member identifiers only --------------------------------------------

void rp_sha1_init(rp_sha1_t *s) { proven_sha1_init(s); }

void rp_sha1_update(rp_sha1_t *s, const void *data, size_t len) {
    proven_sha1_update(s, mv(data, len));
}

void rp_sha1_final(rp_sha1_t *s, uint8_t out[20]) { proven_sha1_final(s, out); }
