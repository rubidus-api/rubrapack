// src/crypto/aes.c - AES-128/192/256 (FIPS 197) and CBC (SP 800-38A), written from the standards.
//
// Constant time: there are no tables indexed by secret bytes. The S-box is computed from its
// definition (FIPS 197 5.1.1): the inverse in GF(2^8), taken as x^254 by a fixed square-and-
// multiply chain, then the affine map; GF multiplication uses masks instead of branches. It is
// slow next to table AES, which does not matter here: AES only decrypts key files (PKCS#12,
// PKCS#8) of a few kilobytes.

#include "rubrapack/crypto.h"

#include <string.h>

static uint8_t gmul(uint8_t a, uint8_t b) {
    uint8_t r = 0;
    for (int i = 0; i < 8; ++i) {
        r ^= (uint8_t)(a & (uint8_t)-(b & 1));
        uint8_t hi = (uint8_t)-(a >> 7);
        a = (uint8_t)((a << 1) ^ (0x1B & hi));
        b >>= 1;
    }
    return r;
}

static uint8_t ginv(uint8_t x) {         // x^254; 0 -> 0
    uint8_t x2 = gmul(x, x), x4 = gmul(x2, x2), x8 = gmul(x4, x4), x16 = gmul(x8, x8), x32 = gmul(x16, x16),
            x64 = gmul(x32, x32), x128 = gmul(x64, x64);
    return gmul(gmul(gmul(gmul(gmul(gmul(x128, x64), x32), x16), x8), x4), x2);
}

static uint8_t rotl8(uint8_t x, int n) { return (uint8_t)((x << n) | (x >> (8 - n))); }

static uint8_t sbox(uint8_t x) {
    uint8_t b = ginv(x);
    return (uint8_t)(b ^ rotl8(b, 1) ^ rotl8(b, 2) ^ rotl8(b, 3) ^ rotl8(b, 4) ^ 0x63);
}

static uint8_t inv_sbox(uint8_t y) {
    uint8_t t = (uint8_t)(rotl8(y, 1) ^ rotl8(y, 3) ^ rotl8(y, 6) ^ 0x05);    // the affine map's inverse
    return ginv(t);
}

bool rp_aes_init(rp_aes_t *a, const uint8_t *key, size_t key_len) {
    if (key_len != 16 && key_len != 24 && key_len != 32) return false;
    int nk = (int)key_len / 4;
    a->rounds = nk + 6;
    int words = 4 * (a->rounds + 1);
    memcpy(a->rk, key, key_len);
    uint8_t rcon = 1;
    for (int i = nk; i < words; ++i) {
        uint8_t t[4];
        memcpy(t, a->rk + 4 * (i - 1), 4);
        if (i % nk == 0) {
            uint8_t t0 = t[0];
            t[0] = (uint8_t)(sbox(t[1]) ^ rcon);
            t[1] = sbox(t[2]);
            t[2] = sbox(t[3]);
            t[3] = sbox(t0);
            rcon = gmul(rcon, 2);
        } else if (nk > 6 && i % nk == 4) {
            for (int j = 0; j < 4; ++j) t[j] = sbox(t[j]);
        }
        for (int j = 0; j < 4; ++j) a->rk[4 * i + j] = (uint8_t)(a->rk[4 * (i - nk) + j] ^ t[j]);
    }
    return true;
}

static void add_key(uint8_t s[16], const uint8_t *k) {
    for (int i = 0; i < 16; ++i) s[i] ^= k[i];
}

void rp_aes_encrypt(const rp_aes_t *a, const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16], t[16];
    memcpy(s, in, 16);
    add_key(s, a->rk);
    for (int r = 1; r <= a->rounds; ++r) {
        for (int i = 0; i < 16; ++i) s[i] = sbox(s[i]);
        for (int c = 0; c < 4; ++c) {           // ShiftRows: row r moves left by r (column-major state)
            for (int row = 0; row < 4; ++row) t[4 * c + row] = s[4 * ((c + row) % 4) + row];
        }
        if (r != a->rounds) {                   // MixColumns
            for (int c = 0; c < 4; ++c) {
                uint8_t *v = t + 4 * c, a0 = v[0], a1 = v[1], a2 = v[2], a3 = v[3];
                v[0] = (uint8_t)(gmul(a0, 2) ^ gmul(a1, 3) ^ a2 ^ a3);
                v[1] = (uint8_t)(a0 ^ gmul(a1, 2) ^ gmul(a2, 3) ^ a3);
                v[2] = (uint8_t)(a0 ^ a1 ^ gmul(a2, 2) ^ gmul(a3, 3));
                v[3] = (uint8_t)(gmul(a0, 3) ^ a1 ^ a2 ^ gmul(a3, 2));
            }
        }
        memcpy(s, t, 16);
        add_key(s, a->rk + 16 * r);
    }
    memcpy(out, s, 16);
    rp_wipe(s, 16);
    rp_wipe(t, 16);
}

void rp_aes_decrypt(const rp_aes_t *a, const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16], t[16];
    memcpy(s, in, 16);
    add_key(s, a->rk + 16 * a->rounds);
    for (int r = a->rounds - 1; r >= 0; --r) {
        for (int c = 0; c < 4; ++c) {           // InvShiftRows
            for (int row = 0; row < 4; ++row) t[4 * ((c + row) % 4) + row] = s[4 * c + row];
        }
        for (int i = 0; i < 16; ++i) t[i] = inv_sbox(t[i]);
        add_key(t, a->rk + 16 * r);
        if (r != 0) {                           // InvMixColumns
            for (int c = 0; c < 4; ++c) {
                uint8_t *v = t + 4 * c, a0 = v[0], a1 = v[1], a2 = v[2], a3 = v[3];
                v[0] = (uint8_t)(gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9));
                v[1] = (uint8_t)(gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13));
                v[2] = (uint8_t)(gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11));
                v[3] = (uint8_t)(gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14));
            }
        }
        memcpy(s, t, 16);
    }
    memcpy(out, s, 16);
    rp_wipe(s, 16);
    rp_wipe(t, 16);
}

bool rp_aes_cbc_encrypt(const rp_aes_t *a, const uint8_t iv[16], const uint8_t *in, uint8_t *out, size_t len) {
    if (len % 16) return false;
    uint8_t chain[16];
    memcpy(chain, iv, 16);
    for (size_t off = 0; off < len; off += 16) {
        uint8_t b[16];
        for (int i = 0; i < 16; ++i) b[i] = (uint8_t)(in[off + (size_t)i] ^ chain[i]);
        rp_aes_encrypt(a, b, chain);
        memcpy(out + off, chain, 16);
    }
    return true;
}

bool rp_aes_cbc_decrypt(const rp_aes_t *a, const uint8_t iv[16], const uint8_t *in, uint8_t *out, size_t len) {
    if (len % 16) return false;
    uint8_t chain[16], next[16], b[16];
    memcpy(chain, iv, 16);
    for (size_t off = 0; off < len; off += 16) {
        memcpy(next, in + off, 16);             // before out overwrites it (in == out allowed)
        rp_aes_decrypt(a, next, b);
        for (int i = 0; i < 16; ++i) out[off + (size_t)i] = (uint8_t)(b[i] ^ chain[i]);
        memcpy(chain, next, 16);
    }
    rp_wipe(b, 16);
    return true;
}
