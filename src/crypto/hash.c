// src/crypto/hash.c - SHA-256 (proven), SHA-384/512 (FIPS 180-4), behind one interface
// (include/rubrapack/crypto.h).
//
// The SHA-512 constants and block function are ported from lowent_lang v1.3.0
// impl/src/low_sha512.h (same author, MIT); the streaming wrapper is new.

#include "rubrapack/crypto.h"

#include <string.h>

static const uint64_t K[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL, 0x3956c25bf348b538ULL,
    0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL, 0xd807aa98a3030242ULL, 0x12835b0145706fbeULL,
    0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL, 0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL,
    0xc19bf174cf692694ULL, 0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL, 0x983e5152ee66dfabULL,
    0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL, 0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL,
    0x06ca6351e003826fULL, 0x142929670a0e6e70ULL, 0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL,
    0x53380d139d95b3dfULL, 0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL, 0xd192e819d6ef5218ULL,
    0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL, 0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL,
    0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL, 0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL,
    0x682e6ff3d6b2b8a3ULL, 0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL, 0xca273eceea26619cULL,
    0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL, 0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL,
    0x113f9804bef90daeULL, 0x1b710b35131c471bULL, 0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL,
    0x431d67c49c100d4cULL, 0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL,
};

static uint64_t rr(uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }

static void block512(uint64_t h[8], const uint8_t *p) {
    uint64_t w[80];
    for (int i = 0; i < 16; ++i) {
        w[i] = 0;
        for (int j = 0; j < 8; ++j) w[i] = (w[i] << 8) | p[i * 8 + j];
    }
    for (int i = 16; i < 80; ++i) {
        uint64_t s0 = rr(w[i - 15], 1) ^ rr(w[i - 15], 8) ^ (w[i - 15] >> 7);
        uint64_t s1 = rr(w[i - 2], 19) ^ rr(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint64_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 80; ++i) {
        uint64_t t1 = hh + (rr(e, 14) ^ rr(e, 18) ^ rr(e, 41)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        uint64_t t2 = (rr(a, 28) ^ rr(a, 34) ^ rr(a, 39)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

size_t rp_hash_size(rp_hash_alg_t alg) { return alg == RP_HASH_SHA256 ? 32 : alg == RP_HASH_SHA384 ? 48 : 64; }
size_t rp_hash_block(rp_hash_alg_t alg) { return alg == RP_HASH_SHA256 ? 64 : 128; }

void rp_hash_init(rp_hash_t *h, rp_hash_alg_t alg) {
    static const uint64_t iv512[8] = { 0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
                                       0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL, 0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL };
    static const uint64_t iv384[8] = { 0xcbbb9d5dc1059ed8ULL, 0x629a292a367cd507ULL, 0x9159015a3070dd17ULL, 0x152fecd8f70e5939ULL,
                                       0x67332667ffc00b31ULL, 0x8eb44a8768581511ULL, 0xdb0c2e0d64f98fa7ULL, 0x47b5481dbefa4fa4ULL };
    memset(h, 0, sizeof *h);
    h->alg = alg;
    if (alg == RP_HASH_SHA256) proven_sha256_init(&h->u.s256);
    else memcpy(h->u.s512.h, alg == RP_HASH_SHA384 ? iv384 : iv512, sizeof iv512);
}

void rp_hash_update(rp_hash_t *h, const void *data, size_t len) {
    if (h->alg == RP_HASH_SHA256) {
        proven_sha256_update(&h->u.s256, (proven_mem_view_t){ (const proven_byte_t *)data, len });
        return;
    }
    const uint8_t *p = data;
    size_t used = (size_t)(h->u.s512.n % 128);
    h->u.s512.n += len;
    if (used) {
        size_t take = 128 - used < len ? 128 - used : len;
        memcpy(h->u.s512.b + used, p, take);
        p += take;
        len -= take;
        if (used + take < 128) return;
        block512(h->u.s512.h, h->u.s512.b);
    }
    for (; len >= 128; p += 128, len -= 128) block512(h->u.s512.h, p);
    memcpy(h->u.s512.b, p, len);
}

void rp_hash_final(rp_hash_t *h, uint8_t *out) {
    if (h->alg == RP_HASH_SHA256) {
        proven_sha256_final(&h->u.s256, out);
        rp_wipe(h, sizeof *h);
        return;
    }
    uint64_t bits = h->u.s512.n * 8;
    size_t used = (size_t)(h->u.s512.n % 128);
    uint8_t pad[256] = { 0x80 };
    size_t padlen = (used < 112 ? 112 : 240) - used;   // up to the 16-byte length field
    uint8_t lenfield[16] = { 0 };
    for (int i = 0; i < 8; ++i) lenfield[15 - i] = (uint8_t)(bits >> (8 * i));
    rp_hash_update(h, pad, padlen);
    rp_hash_update(h, lenfield, 16);
    size_t words = h->alg == RP_HASH_SHA384 ? 6 : 8;
    for (size_t i = 0; i < words; ++i) {
        for (int j = 0; j < 8; ++j) out[i * 8 + (size_t)j] = (uint8_t)(h->u.s512.h[i] >> (56 - 8 * j));
    }
    rp_wipe(h, sizeof *h);
}

void rp_hash(rp_hash_alg_t alg, const void *data, size_t len, uint8_t *out) {
    rp_hash_t h;
    rp_hash_init(&h, alg);
    rp_hash_update(&h, data, len);
    rp_hash_final(&h, out);
}

void rp_wipe(void *p, size_t n) {
    volatile uint8_t *v = p;
    while (n--) *v++ = 0;
}

bool rp_ct_equal(const uint8_t *a, const uint8_t *b, size_t n) {
    uint8_t d = 0;
    for (size_t i = 0; i < n; ++i) d |= (uint8_t)(a[i] ^ b[i]);
    return d == 0;
}

// ---- HMAC, PBKDF2 --------------------------------------------------------------------------

void rp_hmac_init(rp_hmac_t *m, rp_hash_alg_t alg, const uint8_t *key, size_t key_len) {
    uint8_t k[128] = { 0 }, pad[128];
    size_t bs = rp_hash_block(alg);
    if (key_len > bs) rp_hash(alg, key, key_len, k);
    else if (key_len) memcpy(k, key, key_len);
    for (size_t i = 0; i < bs; ++i) pad[i] = (uint8_t)(k[i] ^ 0x36);
    rp_hash_init(&m->inner, alg);
    rp_hash_update(&m->inner, pad, bs);
    for (size_t i = 0; i < bs; ++i) pad[i] = (uint8_t)(k[i] ^ 0x5c);
    rp_hash_init(&m->outer, alg);
    rp_hash_update(&m->outer, pad, bs);
    rp_wipe(k, sizeof k);
    rp_wipe(pad, sizeof pad);
}

void rp_hmac_update(rp_hmac_t *m, const void *data, size_t len) { rp_hash_update(&m->inner, data, len); }

void rp_hmac_final(rp_hmac_t *m, uint8_t *out) {
    uint8_t d[RP_HASH_MAX];
    size_t n = rp_hash_size(m->inner.alg);
    rp_hash_final(&m->inner, d);
    rp_hash_update(&m->outer, d, n);
    rp_hash_final(&m->outer, out);
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

void rp_hkdf_extract(rp_hash_alg_t alg, const uint8_t *salt, size_t salt_len, const uint8_t *ikm, size_t ikm_len, uint8_t *prk) {
    // HMAC pads its key with zeros, so an empty salt already is "HashLen zeros".
    rp_hmac(alg, salt, salt_len, ikm, ikm_len, prk);
}

bool rp_hkdf_expand(rp_hash_alg_t alg, const uint8_t *prk, size_t prk_len, const uint8_t *info, size_t info_len, uint8_t *out,
                    size_t len) {
    size_t hl = rp_hash_size(alg);
    if (len > 255 * hl) return false;
    rp_hmac_t base, m;
    rp_hmac_init(&base, alg, prk, prk_len);
    uint8_t t[RP_HASH_MAX];
    size_t tl = 0;
    for (uint8_t i = 1; len; ++i) {
        m = base;
        rp_hmac_update(&m, t, tl);
        rp_hmac_update(&m, info, info_len);
        rp_hmac_update(&m, &i, 1);
        rp_hmac_final(&m, t);
        tl = hl;
        size_t take = len < hl ? len : hl;
        memcpy(out, t, take);
        out += take;
        len -= take;
    }
    rp_wipe(t, sizeof t);
    rp_wipe(&base, sizeof base);
    rp_wipe(&m, sizeof m);
    return true;
}

// ---- SHA-1 (FIPS 180-4 6.1), for catalog member identifiers only ----------------------------

static uint32_t rl1(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

static void block1(uint32_t h[5], const uint8_t *p) {
    uint32_t w[80];
    for (int t = 0; t < 16; ++t) w[t] = (uint32_t)p[4 * t] << 24 | (uint32_t)p[4 * t + 1] << 16 | (uint32_t)p[4 * t + 2] << 8 | p[4 * t + 3];
    for (int t = 16; t < 80; ++t) w[t] = rl1(w[t - 3] ^ w[t - 8] ^ w[t - 14] ^ w[t - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int t = 0; t < 80; ++t) {
        uint32_t f, k;
        if (t < 20) f = (b & c) | (~b & d), k = 0x5A827999u;
        else if (t < 40) f = b ^ c ^ d, k = 0x6ED9EBA1u;
        else if (t < 60) f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDCu;
        else f = b ^ c ^ d, k = 0xCA62C1D6u;
        uint32_t tmp = rl1(a, 5) + f + e + k + w[t];
        e = d;
        d = c;
        c = rl1(b, 30);
        b = a;
        a = tmp;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
}

void rp_sha1_init(rp_sha1_t *s) {
    static const uint32_t iv[5] = { 0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u };
    memcpy(s->h, iv, sizeof iv);
    s->n = 0;
}

void rp_sha1_update(rp_sha1_t *s, const void *data, size_t len) {
    const uint8_t *p = data;
    size_t fill = (size_t)(s->n % 64);
    s->n += len;
    if (fill) {
        size_t take = 64 - fill < len ? 64 - fill : len;
        memcpy(s->b + fill, p, take);
        p += take;
        len -= take;
        if (fill + take < 64) return;
        block1(s->h, s->b);
    }
    for (; len >= 64; p += 64, len -= 64) block1(s->h, p);
    if (len) memcpy(s->b, p, len);
}

void rp_sha1_final(rp_sha1_t *s, uint8_t out[20]) {
    uint64_t bits = s->n * 8;
    size_t fill = (size_t)(s->n % 64);
    s->b[fill++] = 0x80;
    if (fill > 56) {
        memset(s->b + fill, 0, 64 - fill);
        block1(s->h, s->b);
        fill = 0;
    }
    memset(s->b + fill, 0, 56 - fill);
    for (int i = 0; i < 8; ++i) s->b[56 + i] = (uint8_t)(bits >> (56 - 8 * i));
    block1(s->h, s->b);
    for (int i = 0; i < 5; ++i) {
        out[4 * i] = (uint8_t)(s->h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(s->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(s->h[i] >> 8);
        out[4 * i + 3] = (uint8_t)s->h[i];
    }
}
