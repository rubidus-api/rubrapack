// src/crypto/hash.c - SHA-256/384/512, SHA-1, HMAC, HKDF and PBKDF2 behind one interface
// (include/rubrapack/crypto.h). The hashes, HMAC and HKDF are proven's; PBKDF2, which proven
// does not have (proven/hmac.h), is a loop over proven's HMAC.

#include "rubrapack/crypto.h"

#include <string.h>

static proven_mem_view_t mv(const void *p, size_t n) { return (proven_mem_view_t){ (const proven_byte_t *)p, n }; }

static proven_hmac_hash_t mac_of(rp_hash_alg_t alg) {
    return alg == RP_HASH_SHA256 ? PROVEN_HMAC_SHA256 : alg == RP_HASH_SHA384 ? PROVEN_HMAC_SHA384 : PROVEN_HMAC_SHA512;
}

size_t rp_hash_size(rp_hash_alg_t alg) { return alg == RP_HASH_SHA256 ? 32 : alg == RP_HASH_SHA384 ? 48 : 64; }
size_t rp_hash_block(rp_hash_alg_t alg) { return alg == RP_HASH_SHA256 ? 64 : 128; }

void rp_hash_init(rp_hash_t *h, rp_hash_alg_t alg) {
    h->alg = alg;
    if (alg == RP_HASH_SHA256) proven_sha256_init(&h->u.s256);
    else if (alg == RP_HASH_SHA384) proven_sha384_init(&h->u.s512);
    else proven_sha512_init(&h->u.s512);
}

void rp_hash_update(rp_hash_t *h, const void *data, size_t len) {
    if (len == 0) return;                       // data may be NULL then (an empty stream's buffer)
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

// ---- HMAC, PBKDF2, HKDF --------------------------------------------------------------------

void rp_hmac_init(rp_hmac_t *m, rp_hash_alg_t alg, const uint8_t *key, size_t key_len) {
    // Fails only for an unknown hash or a NULL key with a length, neither of which reaches here.
    (void)proven_hmac_init(&m->m, mac_of(alg), mv(key_len ? key : (const uint8_t *)"", key_len));
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
    size_t hl = rp_hash_size(alg);
    rp_hmac_t base, m;
    rp_hmac_init(&base, alg, pass, pass_len);      // the keyed state, copied for every HMAC
    uint8_t u[RP_HASH_MAX], t[RP_HASH_MAX];
    for (uint32_t block = 1; out_len; ++block) {
        uint8_t ctr[4] = { (uint8_t)(block >> 24), (uint8_t)(block >> 16), (uint8_t)(block >> 8), (uint8_t)block };
        m = base;
        rp_hmac_update(&m, salt, salt_len);
        rp_hmac_update(&m, ctr, 4);
        rp_hmac_final(&m, u);
        memcpy(t, u, hl);
        for (uint32_t i = 1; i < iterations; ++i) {
            m = base;
            rp_hmac_update(&m, u, hl);
            rp_hmac_final(&m, u);
            for (size_t j = 0; j < hl; ++j) t[j] ^= u[j];
        }
        size_t take = out_len < hl ? out_len : hl;
        memcpy(out, t, take);
        out += take;
        out_len -= take;
    }
    rp_wipe(&base, sizeof base);
    rp_wipe(&m, sizeof m);
    rp_wipe(u, sizeof u);
    rp_wipe(t, sizeof t);
}

// ---- SHA-1, for catalog member identifiers only --------------------------------------------

void rp_sha1_init(rp_sha1_t *s) { proven_sha1_init(s); }

void rp_sha1_update(rp_sha1_t *s, const void *data, size_t len) {
    if (len) proven_sha1_update(s, mv(data, len));
}

void rp_sha1_final(rp_sha1_t *s, uint8_t out[20]) { proven_sha1_final(s, out); }
