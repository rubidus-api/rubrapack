// src/crypto/bn.c - Montgomery arithmetic for RSA (include/rubrapack/crypto.h).
//
// The design follows lowent_lang v1.3.0 lib/bigint.low (same author, MIT): Montgomery products
// by CIOS, and R^2 mod n made by doubling 1 with subtractions, so no long division is needed.
// Rewritten here with 32-bit limbs (64-bit products) and extended for signing: every step that
// may see a secret - the final subtraction of a Montgomery product, the table lookup of the
// fixed-window exponentiation - runs the same instructions and touches the same memory whatever
// the values; the loops depend only on the modulus size and the exponent length.

#include "rubrapack/crypto.h"

#include <string.h>

// r = t (k+1 limbs, t < 2n) reduced below n, without a data-dependent branch.
static void cond_sub(const rp_mont_t *m, uint32_t *r, const uint32_t *t, uint32_t top) {
    uint32_t u[RP_BN_LIMBS];
    uint64_t borrow = 0;
    for (size_t j = 0; j < m->k; ++j) {
        uint64_t d = (uint64_t)t[j] - m->n[j] - borrow;
        u[j] = (uint32_t)d;
        borrow = (d >> 63) & 1;
    }
    // Keep the difference when there was no borrow, or when the top limb absorbs it.
    uint32_t use_u = (uint32_t)0 - (uint32_t)((borrow ^ 1) | top);
    for (size_t j = 0; j < m->k; ++j) r[j] = (u[j] & use_u) | (t[j] & ~use_u);
}

// r = a * b * R^-1 mod n (CIOS). r may alias a or b.
static void mont_mul(const rp_mont_t *m, uint32_t *r, const uint32_t *a, const uint32_t *b) {
    uint32_t t[RP_BN_LIMBS + 2] = { 0 };
    size_t k = m->k;
    for (size_t i = 0; i < k; ++i) {
        uint64_t c = 0;
        for (size_t j = 0; j < k; ++j) {
            c += (uint64_t)t[j] + (uint64_t)a[j] * b[i];
            t[j] = (uint32_t)c;
            c >>= 32;
        }
        c += t[k];
        t[k] = (uint32_t)c;
        t[k + 1] = (uint32_t)(c >> 32);
        uint32_t q = t[0] * m->n0inv;
        c = (uint64_t)t[0] + (uint64_t)q * m->n[0];
        c >>= 32;
        for (size_t j = 1; j < k; ++j) {
            c += (uint64_t)t[j] + (uint64_t)q * m->n[j];
            t[j - 1] = (uint32_t)c;
            c >>= 32;
        }
        c += t[k];
        t[k - 1] = (uint32_t)c;
        t[k] = t[k + 1] + (uint32_t)(c >> 32);
    }
    cond_sub(m, r, t, t[k]);
}

static void from_be(uint32_t *v, size_t k, const uint8_t *b, size_t len) {
    memset(v, 0, k * sizeof *v);
    for (size_t i = 0; i < len && i < 4 * k; ++i) v[i / 4] |= (uint32_t)b[len - 1 - i] << (8 * (i % 4));
}

bool rp_mont_init(rp_mont_t *m, const uint8_t *mod, size_t len) {
    memset(m, 0, sizeof *m);
    while (len && mod[0] == 0) {
        ++mod;
        --len;
    }
    if (len == 0 || len > 4 * RP_BN_LIMBS || !(mod[len - 1] & 1)) return false;
    m->k = (len + 3) / 4;
    from_be(m->n, m->k, mod, len);
    m->bits = 32 * m->k;
    while (m->bits && !(m->n[(m->bits - 1) / 32] >> ((m->bits - 1) % 32) & 1)) --m->bits;
    // n0inv = -n^-1 mod 2^32 by Newton's iteration (each step doubles the correct bits).
    uint32_t inv = 1;
    for (int i = 0; i < 5; ++i) inv *= 2 - m->n[0] * inv;
    m->n0inv = (uint32_t)0 - inv;
    // R^2 mod n: 1 doubled 2 * 32k times, reduced after every doubling.
    uint32_t v[RP_BN_LIMBS] = { 1 };
    for (size_t i = 0; i < 64 * m->k; ++i) {
        uint32_t carry = 0;
        for (size_t j = 0; j < m->k; ++j) {
            uint32_t nx = (v[j] >> 31);
            v[j] = (v[j] << 1) | carry;
            carry = nx;
        }
        cond_sub(m, v, v, carry);
    }
    memcpy(m->rr, v, sizeof v);
    return true;
}

void rp_mont_mulmod(const rp_mont_t *m, uint32_t *r, const uint32_t *a, const uint32_t *b) {
    uint32_t t[RP_BN_LIMBS];
    mont_mul(m, t, a, m->rr);       // a R
    mont_mul(m, r, t, b);           // a b
}

void rp_mont_reduce_bytes(const rp_mont_t *m, const uint8_t *x, size_t len, uint32_t *r) {
    // x = hi R + lo with hi, lo < R. mont(v, R^2) = v R mod n for any v < R (v R^2 < R n), so:
    // hi R mod n = mont(hi, R^2), lo mod n = mont(mont(lo, R^2), 1); then one modular addition.
    size_t k = m->k;
    uint32_t full[2 * RP_BN_LIMBS], a[RP_BN_LIMBS], b[RP_BN_LIMBS], one[RP_BN_LIMBS] = { 1 };
    from_be(full, 2 * k, x, len);
    mont_mul(m, a, full, m->rr);            // lo R
    mont_mul(m, a, a, one);                 // lo mod n
    mont_mul(m, b, full + k, m->rr);        // hi R mod n
    uint64_t c = 0;
    for (size_t j = 0; j < k; ++j) {
        c += (uint64_t)a[j] + b[j];
        r[j] = (uint32_t)c;
        c >>= 32;
    }
    cond_sub(m, r, r, (uint32_t)c);
    rp_wipe(full, sizeof full);
    rp_wipe(a, sizeof a);
    rp_wipe(b, sizeof b);
}

// Constant-time select of table[idx] (every entry is read).
static void select_ct(const rp_mont_t *m, uint32_t *out, const uint32_t table[16][RP_BN_LIMBS], uint32_t idx) {
    memset(out, 0, m->k * sizeof *out);
    for (uint32_t i = 0; i < 16; ++i) {
        uint32_t d = i ^ idx;
        uint32_t mask = (uint32_t)0 - (uint32_t)(((uint64_t)d - 1) >> 63);    // all ones when d == 0
        for (size_t j = 0; j < m->k; ++j) out[j] |= table[i][j] & mask;
    }
}

void rp_mont_exp_ct(const rp_mont_t *m, uint32_t *r, const uint32_t *base, const uint8_t *exp, size_t exp_len) {
    uint32_t table[16][RP_BN_LIMBS];
    uint32_t one[RP_BN_LIMBS] = { 1 }, acc[RP_BN_LIMBS], t[RP_BN_LIMBS];
    mont_mul(m, table[0], one, m->rr);          // 1 in Montgomery form
    mont_mul(m, table[1], base, m->rr);         // base in Montgomery form
    for (int i = 2; i < 16; ++i) mont_mul(m, table[i], table[i - 1], table[1]);
    memcpy(acc, table[0], sizeof acc);
    for (size_t i = 0; i < exp_len; ++i) {
        for (int half = 0; half < 2; ++half) {
            uint32_t w = half == 0 ? exp[i] >> 4 : exp[i] & 15;
            for (int s = 0; s < 4; ++s) mont_mul(m, acc, acc, acc);
            select_ct(m, t, (const uint32_t(*)[RP_BN_LIMBS])table, w);
            mont_mul(m, acc, acc, t);
        }
    }
    mont_mul(m, r, acc, one);                   // out of Montgomery form
    rp_wipe(table, sizeof table);
    rp_wipe(acc, sizeof acc);
    rp_wipe(t, sizeof t);
}

void rp_mont_exp_pub(const rp_mont_t *m, uint32_t *r, const uint32_t *base, const uint8_t *exp, size_t exp_len) {
    uint32_t one[RP_BN_LIMBS] = { 1 }, acc[RP_BN_LIMBS], b[RP_BN_LIMBS];
    mont_mul(m, acc, one, m->rr);
    mont_mul(m, b, base, m->rr);
    for (size_t i = 0; i < exp_len; ++i) {
        for (int bit = 7; bit >= 0; --bit) {
            mont_mul(m, acc, acc, acc);
            if (exp[i] >> bit & 1) mont_mul(m, acc, acc, b);
        }
    }
    mont_mul(m, r, acc, one);
}
