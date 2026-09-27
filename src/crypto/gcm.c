// src/crypto/gcm.c - AES-GCM (NIST SP 800-38D; include/rubrapack/crypto.h) for 96-bit IVs and
// 128-bit tags, what TLS 1.3 uses.
//
// GHASH multiplies in GF(2^128) bit by bit (SP 800-38D 6.3, algorithm 1) with masks instead of
// branches and without tables: the time does not depend on H or on the data, which a 4-bit or
// 8-bit table indexed by secret bytes would not guarantee. It is slow next to table or carry-less
// multiply versions, and fast enough for what rubrapack sends: timestamp requests and answers.

#include "rubrapack/crypto.h"

#include <string.h>

static uint64_t be64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return v;
}

static void put64(uint8_t *p, uint64_t v) {
    for (int i = 7; i >= 0; --i) {
        p[i] = (uint8_t)v;
        v >>= 8;
    }
}

// (x_hi, x_lo) = X * H in GF(2^128), SP 800-38D's bit order (bit 0 = the most significant bit of
// the first byte), R = 11100001 || 0^120.
static void gmul(const rp_gcm_t *g, uint64_t *x_hi, uint64_t *x_lo) {
    uint64_t z_hi = 0, z_lo = 0, v_hi = g->h_hi, v_lo = g->h_lo, xh = *x_hi, xl = *x_lo;
    for (int i = 0; i < 128; ++i) {
        uint64_t bit = i < 64 ? (xh >> (63 - i)) & 1 : (xl >> (127 - i)) & 1;
        uint64_t m = (uint64_t)0 - bit;
        z_hi ^= v_hi & m;
        z_lo ^= v_lo & m;
        uint64_t r = (uint64_t)0 - (v_lo & 1);
        v_lo = (v_lo >> 1) | (v_hi << 63);
        v_hi = (v_hi >> 1) ^ (0xE100000000000000ULL & r);
    }
    *x_hi = z_hi;
    *x_lo = z_lo;
}

static void ghash(const rp_gcm_t *g, uint64_t *y_hi, uint64_t *y_lo, const uint8_t *data, size_t len) {
    while (len) {
        uint8_t block[16] = { 0 };
        size_t take = len < 16 ? len : 16;
        memcpy(block, data, take);
        *y_hi ^= be64(block);
        *y_lo ^= be64(block + 8);
        gmul(g, y_hi, y_lo);
        data += take;
        len -= take;
    }
}

bool rp_gcm_init(rp_gcm_t *g, const uint8_t *key, size_t key_len) {
    if (!rp_aes_init(&g->aes, key, key_len)) return false;
    uint8_t zero[16] = { 0 }, h[16];
    rp_aes_encrypt(&g->aes, zero, h);
    g->h_hi = be64(h);
    g->h_lo = be64(h + 8);
    rp_wipe(h, sizeof h);
    return true;
}

// CTR from J0 + 1 (inc32), and the tag's mask E(K, J0).
static void ctr(const rp_gcm_t *g, const uint8_t iv[12], const uint8_t *in, size_t len, uint8_t *out, uint8_t ek0[16]) {
    uint8_t cb[16], ks[16];
    memcpy(cb, iv, 12);
    cb[12] = cb[13] = cb[14] = 0;
    cb[15] = 1;
    rp_aes_encrypt(&g->aes, cb, ek0);
    uint32_t n = 1;
    for (size_t off = 0; off < len; off += 16) {
        ++n;
        cb[12] = (uint8_t)(n >> 24);
        cb[13] = (uint8_t)(n >> 16);
        cb[14] = (uint8_t)(n >> 8);
        cb[15] = (uint8_t)n;
        rp_aes_encrypt(&g->aes, cb, ks);
        size_t take = len - off < 16 ? len - off : 16;
        for (size_t i = 0; i < take; ++i) out[off + i] = in[off + i] ^ ks[i];
    }
    rp_wipe(ks, sizeof ks);
}

static void tag_of(const rp_gcm_t *g, const uint8_t *aad, size_t aad_len, const uint8_t *c, size_t len, const uint8_t ek0[16],
                   uint8_t tag[16]) {
    uint64_t y_hi = 0, y_lo = 0;
    ghash(g, &y_hi, &y_lo, aad, aad_len);
    ghash(g, &y_hi, &y_lo, c, len);
    uint8_t lens[16];
    put64(lens, (uint64_t)aad_len * 8);
    put64(lens + 8, (uint64_t)len * 8);
    ghash(g, &y_hi, &y_lo, lens, 16);
    put64(tag, y_hi);
    put64(tag + 8, y_lo);
    for (int i = 0; i < 16; ++i) tag[i] ^= ek0[i];
}

void rp_gcm_seal(const rp_gcm_t *g, const uint8_t iv[12], const uint8_t *aad, size_t aad_len, const uint8_t *in, size_t len,
                 uint8_t *out, uint8_t tag[16]) {
    uint8_t ek0[16];
    ctr(g, iv, in, len, out, ek0);
    tag_of(g, aad, aad_len, out, len, ek0, tag);
    rp_wipe(ek0, sizeof ek0);
}

bool rp_gcm_open(const rp_gcm_t *g, const uint8_t iv[12], const uint8_t *aad, size_t aad_len, const uint8_t *in, size_t len,
                 const uint8_t tag[16], uint8_t *out) {
    uint8_t ek0[16], want[16];
    ctr(g, iv, NULL, 0, NULL, ek0);                    // E(K, J0) only
    tag_of(g, aad, aad_len, in, len, ek0, want);
    bool ok = rp_ct_equal(want, tag, 16);
    if (ok) ctr(g, iv, in, len, out, ek0);
    rp_wipe(ek0, sizeof ek0);
    return ok;
}
