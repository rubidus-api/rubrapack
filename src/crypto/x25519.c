// src/crypto/x25519.c - X25519 (RFC 7748 5; include/rubrapack/crypto.h).
//
// Field elements of GF(2^255 - 19) as 16 signed 64-bit limbs of 16 bits each (radix 2^16), the
// representation of the public-domain TweetNaCl design: products fit in 64 bits without a wider
// type, and reduction folds 2^256 = 38 (mod p). The Montgomery ladder of RFC 7748 5 runs a fixed
// 255 steps with a masked swap, and no branch or memory address depends on the scalar.

#include "rubrapack/crypto.h"

#include <string.h>

typedef int64_t fe[16];

// Brings every limb back to 16 bits, carrying up; the top carry wraps around times 38 (2 * 19).
static void carry(fe o) {
    for (int i = 0; i < 16; ++i) {
        o[i] += (int64_t)1 << 16;
        // floor(o[i] / 2^16) without shifting a negative value (implementation-defined in C):
        // limbs stay far below 2^62 in magnitude, so the bias keeps the shifted value positive.
        int64_t c = (int64_t)(((uint64_t)o[i] + ((uint64_t)1 << 62)) >> 16) - ((int64_t)1 << 46);
        if (i < 15) o[i + 1] += c - 1;
        else o[0] += 38 * (c - 1);
        o[i] -= c * 65536;
    }
}

// Swaps p and q when b is 1, leaves them when b is 0, the same way either time.
static void cswap(fe p, fe q, int64_t b) {
    int64_t mask = -b;
    for (int i = 0; i < 16; ++i) {
        int64_t t = mask & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

// The canonical 32 bytes (little-endian, fully reduced mod p).
static void pack(uint8_t out[32], const fe n) {
    fe t, m;
    memcpy(t, n, sizeof t);
    carry(t);
    carry(t);
    carry(t);
    for (int j = 0; j < 2; ++j) {                      // subtract p when t >= p, without a branch
        m[0] = t[0] - 0xFFED;
        for (int i = 1; i < 15; ++i) {
            m[i] = t[i] - 0xFFFF - (int64_t)(((uint64_t)m[i - 1] >> 16) & 1);   // the borrow bit
            m[i - 1] &= 0xFFFF;
        }
        m[15] = t[15] - 0x7FFF - (int64_t)(((uint64_t)m[14] >> 16) & 1);
        int64_t borrow = (int64_t)(((uint64_t)m[15] >> 16) & 1);
        m[14] &= 0xFFFF;
        cswap(t, m, 1 - borrow);
    }
    for (int i = 0; i < 16; ++i) {
        out[2 * i] = (uint8_t)(t[i] & 0xFF);
        out[2 * i + 1] = (uint8_t)(((uint64_t)t[i] >> 8) & 0xFF);
    }
}

// The u-coordinate, its top bit ignored (RFC 7748 5: "mask the most significant bit").
static void unpack(fe o, const uint8_t in[32]) {
    for (int i = 0; i < 16; ++i) o[i] = in[2 * i] + ((int64_t)in[2 * i + 1] << 8);
    o[15] &= 0x7FFF;
}

static void add(fe o, const fe a, const fe b) {
    for (int i = 0; i < 16; ++i) o[i] = a[i] + b[i];
}

static void sub(fe o, const fe a, const fe b) {
    for (int i = 0; i < 16; ++i) o[i] = a[i] - b[i];
}

static void mul(fe o, const fe a, const fe b) {
    int64_t t[31] = { 0 };
    for (int i = 0; i < 16; ++i) {
        for (int j = 0; j < 16; ++j) t[i + j] += a[i] * b[j];
    }
    for (int i = 0; i < 15; ++i) t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; ++i) o[i] = t[i];
    carry(o);
    carry(o);
}

// o = i^(p-2) = 1/i (Fermat): p - 2 = 2^255 - 21, every bit set but bits 2 and 4.
static void invert(fe o, const fe i) {
    fe c;
    memcpy(c, i, sizeof c);
    for (int a = 253; a >= 0; --a) {
        mul(c, c, c);
        if (a != 2 && a != 4) mul(c, c, i);
    }
    memcpy(o, c, sizeof c);
}

bool rp_x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t u[32]) {
    uint8_t k[32];
    memcpy(k, scalar, 32);
    k[0] &= 248;                                       // decodeScalar25519
    k[31] = (uint8_t)((k[31] & 127) | 64);
    static const fe a24 = { 0xDB41, 1 };               // (486662 - 2) / 4 = 121665
    fe x1, x2 = { 1 }, z2 = { 0 }, x3, z3 = { 1 }, a, b, c, d, e, f;
    unpack(x1, u);
    memcpy(x3, x1, sizeof x3);
    int64_t swap = 0;
    for (int t = 254; t >= 0; --t) {
        int64_t bit = (k[t >> 3] >> (t & 7)) & 1;
        swap ^= bit;
        cswap(x2, x3, swap);
        cswap(z2, z3, swap);
        swap = bit;
        add(a, x2, z2);                                // RFC 7748 5, the ladder step
        sub(b, x2, z2);
        add(c, x3, z3);
        sub(d, x3, z3);
        mul(d, d, a);                                  // DA
        mul(c, c, b);                                  // CB
        mul(a, a, a);                                  // AA
        mul(b, b, b);                                  // BB
        sub(e, a, b);                                  // E = AA - BB
        add(f, d, c);
        mul(x3, f, f);                                 // x3 = (DA + CB)^2
        sub(f, d, c);
        mul(f, f, f);
        mul(z3, x1, f);                                // z3 = x1 * (DA - CB)^2
        mul(x2, a, b);                                 // x2 = AA * BB
        mul(f, a24, e);
        add(f, a, f);
        mul(z2, e, f);                                 // z2 = E * (AA + a24 * E)
    }
    cswap(x2, x3, swap);
    cswap(z2, z3, swap);
    invert(z2, z2);
    mul(x2, x2, z2);
    pack(out, x2);
    uint8_t any = 0;
    for (int i = 0; i < 32; ++i) any |= out[i];
    rp_wipe(k, sizeof k);
    rp_wipe(x2, sizeof x2);
    rp_wipe(x3, sizeof x3);
    rp_wipe(z2, sizeof z2);
    rp_wipe(z3, sizeof z3);
    return any != 0;
}
