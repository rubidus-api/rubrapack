// src/crypto/ecdsa.c - ECDSA over NIST P-256 and P-384 (include/rubrapack/crypto.h).
//
// Port of lowent_lang v1.3.0 lib/ecdsa.low + lib/p256.low (same author, MIT): deterministic nonces
// (RFC 6979, HMAC-DRBG over the key and the message hash - no random source to fail), and a scalar
// multiplication that does not branch on the scalar. Changed in the port:
//  - field and scalar arithmetic reuse the constant-time Montgomery code of bn.c (lowent's did not
//    promise a constant-time final subtraction; this one does);
//  - points use the complete projective addition formulas for a = -3 (Renes, Costello, Batina,
//    "Complete addition formulas for prime order elliptic curves", 2016, algorithm 4), so doubling
//    and the point at infinity need no special case - and no branch;
//  - the multiplication is a fixed 4-bit window whose table is read in full at every step;
//  - P-384 uses the same code with its own constants (lowent's p384.low is the same curve shape).
// Curve constants: FIPS 186-4 D.1.2.3/D.1.2.4, checked against Python cryptography's generator
// points and RFC 6979's group orders.

#include "rubrapack/crypto.h"

#include <string.h>

typedef struct {
    size_t        len;              // bytes of p and n
    const uint8_t *p, *b, *gx, *gy, *n;
} curve_t;

static const uint8_t P256_P[] = { 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                  0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
static const uint8_t P256_B[] = { 0x5A, 0xC6, 0x35, 0xD8, 0xAA, 0x3A, 0x93, 0xE7, 0xB3, 0xEB, 0xBD, 0x55, 0x76, 0x98, 0x86, 0xBC,
                                  0x65, 0x1D, 0x06, 0xB0, 0xCC, 0x53, 0xB0, 0xF6, 0x3B, 0xCE, 0x3C, 0x3E, 0x27, 0xD2, 0x60, 0x4B };
static const uint8_t P256_GX[] = { 0x6B, 0x17, 0xD1, 0xF2, 0xE1, 0x2C, 0x42, 0x47, 0xF8, 0xBC, 0xE6, 0xE5, 0x63, 0xA4, 0x40, 0xF2,
                                   0x77, 0x03, 0x7D, 0x81, 0x2D, 0xEB, 0x33, 0xA0, 0xF4, 0xA1, 0x39, 0x45, 0xD8, 0x98, 0xC2, 0x96 };
static const uint8_t P256_GY[] = { 0x4F, 0xE3, 0x42, 0xE2, 0xFE, 0x1A, 0x7F, 0x9B, 0x8E, 0xE7, 0xEB, 0x4A, 0x7C, 0x0F, 0x9E, 0x16,
                                   0x2B, 0xCE, 0x33, 0x57, 0x6B, 0x31, 0x5E, 0xCE, 0xCB, 0xB6, 0x40, 0x68, 0x37, 0xBF, 0x51, 0xF5 };
static const uint8_t P256_N[] = { 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                  0xBC, 0xE6, 0xFA, 0xAD, 0xA7, 0x17, 0x9E, 0x84, 0xF3, 0xB9, 0xCA, 0xC2, 0xFC, 0x63, 0x25, 0x51 };
static const uint8_t P384_P[] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFE,
                                  0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF };
static const uint8_t P384_B[] = { 0xB3, 0x31, 0x2F, 0xA7, 0xE2, 0x3E, 0xE7, 0xE4, 0x98, 0x8E, 0x05, 0x6B, 0xE3, 0xF8, 0x2D, 0x19,
                                  0x18, 0x1D, 0x9C, 0x6E, 0xFE, 0x81, 0x41, 0x12, 0x03, 0x14, 0x08, 0x8F, 0x50, 0x13, 0x87, 0x5A,
                                  0xC6, 0x56, 0x39, 0x8D, 0x8A, 0x2E, 0xD1, 0x9D, 0x2A, 0x85, 0xC8, 0xED, 0xD3, 0xEC, 0x2A, 0xEF };
static const uint8_t P384_GX[] = { 0xAA, 0x87, 0xCA, 0x22, 0xBE, 0x8B, 0x05, 0x37, 0x8E, 0xB1, 0xC7, 0x1E, 0xF3, 0x20, 0xAD, 0x74,
                                   0x6E, 0x1D, 0x3B, 0x62, 0x8B, 0xA7, 0x9B, 0x98, 0x59, 0xF7, 0x41, 0xE0, 0x82, 0x54, 0x2A, 0x38,
                                   0x55, 0x02, 0xF2, 0x5D, 0xBF, 0x55, 0x29, 0x6C, 0x3A, 0x54, 0x5E, 0x38, 0x72, 0x76, 0x0A, 0xB7 };
static const uint8_t P384_GY[] = { 0x36, 0x17, 0xDE, 0x4A, 0x96, 0x26, 0x2C, 0x6F, 0x5D, 0x9E, 0x98, 0xBF, 0x92, 0x92, 0xDC, 0x29,
                                   0xF8, 0xF4, 0x1D, 0xBD, 0x28, 0x9A, 0x14, 0x7C, 0xE9, 0xDA, 0x31, 0x13, 0xB5, 0xF0, 0xB8, 0xC0,
                                   0x0A, 0x60, 0xB1, 0xCE, 0x1D, 0x7E, 0x81, 0x9D, 0x7A, 0x43, 0x1D, 0x7C, 0x90, 0xEA, 0x0E, 0x5F };
static const uint8_t P384_N[] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xC7, 0x63, 0x4D, 0x81, 0xF4, 0x37, 0x2D, 0xDF,
                                  0x58, 0x1A, 0x0D, 0xB2, 0x48, 0xB0, 0xA7, 0x7A, 0xEC, 0xEC, 0x19, 0x6A, 0xCC, 0xC5, 0x29, 0x73 };

static const curve_t CURVES[2] = {
    { 32, P256_P, P256_B, P256_GX, P256_GY, P256_N },
    { 48, P384_P, P384_B, P384_GX, P384_GY, P384_N },
};

enum { L = 12 };        // limbs: 384 bits

typedef struct {
    uint32_t x[L], y[L], z[L];      // projective, Montgomery form
} pt_t;

typedef struct {
    const curve_t *c;
    rp_mont_t      fp, fn;
    uint32_t       b[L];            // b, Montgomery form mod p
    uint32_t       one[L];          // 1, Montgomery form mod p
} ctx_t;

size_t rp_ec_size(rp_ec_curve_t curve) { return CURVES[curve].len; }

static void from_be(uint32_t *v, size_t k, const uint8_t *b, size_t len) {
    memset(v, 0, k * sizeof *v);
    for (size_t i = 0; i < len && i < 4 * k; ++i) v[i / 4] |= (uint32_t)b[len - 1 - i] << (8 * (i % 4));
}

static void to_be(const uint32_t *v, uint8_t *out, size_t len) {
    for (size_t i = 0; i < len; ++i) out[len - 1 - i] = (uint8_t)(v[i / 4] >> (8 * (i % 4)));
}

static bool ctx_init(ctx_t *x, rp_ec_curve_t curve) {
    memset(x, 0, sizeof *x);
    x->c = &CURVES[curve];
    if (!rp_mont_init(&x->fp, x->c->p, x->c->len) || !rp_mont_init(&x->fn, x->c->n, x->c->len)) return false;
    uint32_t t[L];
    from_be(t, x->fp.k, x->c->b, x->c->len);
    rp_mont_to(&x->fp, x->b, t);
    uint32_t one[L] = { 1 };
    rp_mont_to(&x->fp, x->one, one);
    return true;
}

// Complete addition, a = -3 (RCB16 algorithm 4). r may alias p or q.
static void padd(const ctx_t *c, pt_t *r, const pt_t *p, const pt_t *q) {
    const rp_mont_t *m = &c->fp;
    uint32_t t0[L], t1[L], t2[L], t3[L], t4[L], x3[L], y3[L], z3[L];
#define MUL(a, b, o) rp_mont_mul(m, o, a, b)
#define ADD(a, b, o) rp_mont_add(m, o, a, b)
#define SUB(a, b, o) rp_mont_sub(m, o, a, b)
    MUL(p->x, q->x, t0); MUL(p->y, q->y, t1); MUL(p->z, q->z, t2);
    ADD(p->x, p->y, t3); ADD(q->x, q->y, t4); MUL(t3, t4, t3);
    ADD(t0, t1, t4); SUB(t3, t4, t3); ADD(p->y, p->z, t4);
    ADD(q->y, q->z, x3); MUL(t4, x3, t4); ADD(t1, t2, x3);
    SUB(t4, x3, t4); ADD(p->x, p->z, x3); ADD(q->x, q->z, y3);
    MUL(x3, y3, x3); ADD(t0, t2, y3); SUB(x3, y3, y3);
    MUL(c->b, t2, z3); SUB(y3, z3, x3); ADD(x3, x3, z3);
    ADD(x3, z3, x3); SUB(t1, x3, z3); ADD(t1, x3, x3);
    MUL(c->b, y3, y3); ADD(t2, t2, t1); ADD(t1, t2, t2);
    SUB(y3, t2, y3); SUB(y3, t0, y3); ADD(y3, y3, t1);
    ADD(t1, y3, y3); ADD(t0, t0, t1); ADD(t1, t0, t0);
    SUB(t0, t2, t0); MUL(t4, y3, t1); MUL(t0, y3, t2);
    MUL(x3, z3, y3); ADD(y3, t2, y3); MUL(t3, x3, x3);
    SUB(x3, t1, x3); MUL(t4, z3, z3); MUL(t3, t0, t1);
    ADD(z3, t1, z3);
#undef MUL
#undef ADD
#undef SUB
    memcpy(r->x, x3, sizeof x3);
    memcpy(r->y, y3, sizeof y3);
    memcpy(r->z, z3, sizeof z3);
}

static void identity(const ctx_t *c, pt_t *r) {
    memset(r, 0, sizeof *r);
    memcpy(r->y, c->one, sizeof r->y);          // (0 : 1 : 0)
}

// r = k P for a big-endian scalar of the curve's length; the time does not depend on k.
static void smul(const ctx_t *c, pt_t *r, const pt_t *p, const uint8_t *k) {
    pt_t table[16], acc, t;
    identity(c, &table[0]);
    table[1] = *p;
    for (int i = 2; i < 16; ++i) padd(c, &table[i], &table[i - 1], p);
    identity(c, &acc);
    for (size_t i = 0; i < c->c->len; ++i) {
        for (int half = 0; half < 2; ++half) {
            uint32_t w = half == 0 ? k[i] >> 4 : k[i] & 15;
            for (int s = 0; s < 4; ++s) padd(c, &acc, &acc, &acc);
            memset(&t, 0, sizeof t);
            for (uint32_t e = 0; e < 16; ++e) {         // read every entry, keep one
                uint32_t mask = (uint32_t)0 - (uint32_t)(((uint64_t)(e ^ w) - 1) >> 63);
                const uint32_t *src = (const uint32_t *)&table[e];
                uint32_t *dst = (uint32_t *)&t;
                for (size_t j = 0; j < 3 * L; ++j) dst[j] |= src[j] & mask;
            }
            padd(c, &acc, &acc, &t);
        }
    }
    *r = acc;
    rp_wipe(table, sizeof table);
    rp_wipe(&t, sizeof t);
}

// Affine coordinates in normal form; false for the point at infinity.
static bool to_affine(const ctx_t *c, const pt_t *p, uint32_t *x, uint32_t *y) {
    uint32_t z[L], zi[L], pm2b_l[L];
    uint8_t pm2[48];
    rp_mont_from(&c->fp, z, p->z);
    uint32_t any = 0;
    for (size_t j = 0; j < c->fp.k; ++j) any |= z[j];
    if (!any) return false;
    memcpy(pm2, c->c->p, c->c->len);                 // p - 2 (p ends in 0xFF...F)
    pm2[c->c->len - 1] = (uint8_t)(pm2[c->c->len - 1] - 2);
    rp_mont_exp_ct(&c->fp, zi, z, pm2, c->c->len);  // z^-1 (normal form)
    rp_mont_to(&c->fp, pm2b_l, zi);
    uint32_t xm[L], ym[L];
    rp_mont_mul(&c->fp, xm, p->x, pm2b_l);
    rp_mont_mul(&c->fp, ym, p->y, pm2b_l);
    rp_mont_from(&c->fp, x, xm);
    rp_mont_from(&c->fp, y, ym);
    return true;
}

static void from_affine(const ctx_t *c, pt_t *p, const uint32_t *x, const uint32_t *y) {
    rp_mont_to(&c->fp, p->x, x);
    rp_mont_to(&c->fp, p->y, y);
    memcpy(p->z, c->one, sizeof p->z);
}

static void generator(const ctx_t *c, pt_t *g) {
    uint32_t x[L], y[L];
    from_be(x, c->fp.k, c->c->gx, c->c->len);
    from_be(y, c->fp.k, c->c->gy, c->c->len);
    from_affine(c, g, x, y);
}

// 0 < v < n for a big-endian value of the curve's length (public inputs only).
static bool in_range(const ctx_t *c, const uint8_t *v) {
    bool zero = true;
    for (size_t i = 0; i < c->c->len; ++i) zero &= v[i] == 0;
    return !zero && memcmp(v, c->c->n, c->c->len) < 0;
}

// Whether (x, y) with 0 <= x, y < p is on the curve: y^2 = x^3 - 3x + b.
static bool on_curve(const ctx_t *c, const uint8_t *qx, const uint8_t *qy) {
    if (memcmp(qx, c->c->p, c->c->len) >= 0 || memcmp(qy, c->c->p, c->c->len) >= 0) return false;
    uint32_t x[L], y[L], xm[L], ym[L], l[L], r[L], t[L];
    from_be(x, c->fp.k, qx, c->c->len);
    from_be(y, c->fp.k, qy, c->c->len);
    rp_mont_to(&c->fp, xm, x);
    rp_mont_to(&c->fp, ym, y);
    rp_mont_mul(&c->fp, l, ym, ym);
    rp_mont_mul(&c->fp, r, xm, xm);
    rp_mont_mul(&c->fp, r, r, xm);
    rp_mont_add(&c->fp, t, xm, xm);
    rp_mont_add(&c->fp, t, t, xm);
    rp_mont_sub(&c->fp, r, r, t);
    rp_mont_add(&c->fp, r, r, c->b);
    return memcmp(l, r, c->fp.k * sizeof *l) == 0;
}

bool rp_ec_public(rp_ec_curve_t curve, const uint8_t *d, uint8_t *qx, uint8_t *qy) {
    ctx_t c;
    if (!ctx_init(&c, curve) || !in_range(&c, d)) return false;
    pt_t g, q;
    generator(&c, &g);
    smul(&c, &q, &g, d);
    uint32_t x[L], y[L];
    if (!to_affine(&c, &q, x, y)) return false;
    to_be(x, qx, c.c->len);
    to_be(y, qy, c.c->len);
    return true;
}

bool rp_ecdh(rp_ec_curve_t curve, const uint8_t *d, const uint8_t *px, const uint8_t *py, uint8_t *shared_x) {
    ctx_t c;
    if (!ctx_init(&c, curve) || !in_range(&c, d) || !on_curve(&c, px, py)) return false;
    uint32_t x[L], y[L];
    from_be(x, c.fp.k, px, c.c->len);
    from_be(y, c.fp.k, py, c.c->len);
    pt_t p, q;
    from_affine(&c, &p, x, y);
    smul(&c, &q, &p, d);
    bool ok = to_affine(&c, &q, x, y);
    if (ok) to_be(x, shared_x, c.c->len);
    rp_wipe(&q, sizeof q);
    rp_wipe(x, sizeof x);
    return ok;
}

// bits2int (RFC 6979 2.3.2) then mod n: the leftmost qlen bits of the hash, as a scalar.
static void hash_to_scalar(const ctx_t *c, const uint8_t *digest, size_t hl, uint8_t *out) {
    size_t len = c->c->len;
    uint8_t buf[64 + 48] = { 0 };
    if (hl >= len) memcpy(buf, digest, len);                    // P-256/P-384 lengths are whole bytes
    else memcpy(buf + (len - hl), digest, hl);
    uint32_t v[L], r[L];
    from_be(v, c->fn.k, buf, len);
    rp_mont_reduce_bytes(&c->fn, buf, len, r);                 // < 2n, so one reduction suffices
    (void)v;
    to_be(r, out, len);
}

// RFC 6979 3.2: k from HMAC-DRBG(x || h1), tried until 0 < k < n.
static void nonce(const ctx_t *c, rp_hash_alg_t alg, const uint8_t *d, const uint8_t *h, uint8_t *k, uint32_t attempt) {
    size_t hl = rp_hash_size(alg), len = c->c->len;
    uint8_t v[RP_HASH_MAX], key[RP_HASH_MAX], t[96];
    memset(v, 0x01, hl);
    memset(key, 0x00, hl);
    for (int round = 0; round < 2; ++round) {
        uint8_t sep = (uint8_t)round;
        rp_hmac_t m;
        rp_hmac_init(&m, alg, key, hl);
        rp_hmac_update(&m, v, hl);
        rp_hmac_update(&m, &sep, 1);
        rp_hmac_update(&m, d, len);
        rp_hmac_update(&m, h, len);
        rp_hmac_final(&m, key);
        rp_hmac(alg, key, hl, v, hl, v);
    }
    for (uint32_t tries = 0;; ++tries) {
        size_t got = 0;
        while (got < len) {
            rp_hmac(alg, key, hl, v, hl, v);
            size_t take = len - got < hl ? len - got : hl;
            memcpy(t + got, v, take);
            got += take;
        }
        if (in_range(c, t) && tries >= attempt) {
            memcpy(k, t, len);
            break;
        }
        uint8_t zero = 0;
        rp_hmac_t m;
        rp_hmac_init(&m, alg, key, hl);
        rp_hmac_update(&m, v, hl);
        rp_hmac_update(&m, &zero, 1);
        rp_hmac_final(&m, key);
        rp_hmac(alg, key, hl, v, hl, v);
    }
    rp_wipe(v, sizeof v);
    rp_wipe(key, sizeof key);
    rp_wipe(t, sizeof t);
}

proven_err_t rp_ecdsa_sign(rp_ec_curve_t curve, const uint8_t *d, rp_hash_alg_t alg, const uint8_t *digest, uint8_t *r, uint8_t *s) {
    ctx_t c;
    if (!ctx_init(&c, curve) || !in_range(&c, d)) return PROVEN_ERR_INVALID_ARG;
    size_t len = c.c->len;
    uint8_t h[48], k[48], nm2[48], rb[48];
    hash_to_scalar(&c, digest, rp_hash_size(alg), h);
    memcpy(nm2, c.c->n, len);                           // n - 2 (n is odd and ends above 2)
    nm2[len - 1] = (uint8_t)(nm2[len - 1] - 2);
    pt_t g, kg;
    generator(&c, &g);
    for (uint32_t attempt = 0; attempt < 8; ++attempt) {
        nonce(&c, alg, d, h, k, attempt);
        smul(&c, &kg, &g, k);
        uint32_t x[L], y[L];
        if (!to_affine(&c, &kg, x, y)) continue;
        // r = x mod n.
        uint8_t xb[48];
        to_be(x, xb, len);
        uint32_t rv[L], dv[L], hv[L], kv[L], ki[L], t[L], sv[L];
        rp_mont_reduce_bytes(&c.fn, xb, len, rv);
        to_be(rv, rb, len);
        if (!in_range(&c, rb)) continue;
        // s = k^-1 (h + r d) mod n.
        from_be(dv, c.fn.k, d, len);
        from_be(hv, c.fn.k, h, len);
        from_be(kv, c.fn.k, k, len);
        rp_mont_exp_ct(&c.fn, ki, kv, nm2, len);
        rp_mont_mulmod(&c.fn, t, rv, dv);
        uint32_t tm[L], hm[L], sm[L], kim[L];
        rp_mont_to(&c.fn, tm, t);
        rp_mont_to(&c.fn, hm, hv);
        rp_mont_add(&c.fn, tm, tm, hm);
        rp_mont_to(&c.fn, kim, ki);
        rp_mont_mul(&c.fn, sm, tm, kim);
        rp_mont_from(&c.fn, sv, sm);
        to_be(sv, s, len);
        rp_wipe(dv, sizeof dv);
        rp_wipe(kv, sizeof kv);
        rp_wipe(ki, sizeof ki);
        rp_wipe(kim, sizeof kim);
        if (!in_range(&c, s)) continue;
        memcpy(r, rb, len);
        rp_wipe(k, sizeof k);
        // Checked with the public key before it leaves (a fault or a wrong key never does).
        uint8_t qx[48], qy[48];
        if (!rp_ec_public(curve, d, qx, qy) || !rp_ecdsa_verify(curve, qx, qy, alg, digest, r, s)) return PROVEN_ERR_INVALID_STATE;
        return PROVEN_OK;
    }
    return PROVEN_ERR_INVALID_STATE;
}

bool rp_ecdsa_verify(rp_ec_curve_t curve, const uint8_t *qx, const uint8_t *qy, rp_hash_alg_t alg, const uint8_t *digest,
                     const uint8_t *r, const uint8_t *s) {
    ctx_t c;
    if (!ctx_init(&c, curve) || !in_range(&c, r) || !in_range(&c, s) || !on_curve(&c, qx, qy)) return false;
    size_t len = c.c->len;
    uint8_t h[48] = { 0 }, nm2[48] = { 0 }, u1[48] = { 0 }, u2[48] = { 0 };
    hash_to_scalar(&c, digest, rp_hash_size(alg), h);
    memcpy(nm2, c.c->n, len);
    nm2[len - 1] = (uint8_t)(nm2[len - 1] - 2);
    uint32_t sv[L], w[L], hv[L], rv[L], a[L], b[L];
    from_be(sv, c.fn.k, s, len);
    from_be(hv, c.fn.k, h, len);
    from_be(rv, c.fn.k, r, len);
    rp_mont_exp_pub(&c.fn, w, sv, nm2, len);            // s^-1
    rp_mont_mulmod(&c.fn, a, hv, w);
    rp_mont_mulmod(&c.fn, b, rv, w);
    to_be(a, u1, len);
    to_be(b, u2, len);
    pt_t g, q, p1, p2, sum;
    generator(&c, &g);
    uint32_t x[L], y[L];
    from_be(x, c.fp.k, qx, len);
    from_be(y, c.fp.k, qy, len);
    from_affine(&c, &q, x, y);
    smul(&c, &p1, &g, u1);
    smul(&c, &p2, &q, u2);
    padd(&c, &sum, &p1, &p2);
    if (!to_affine(&c, &sum, x, y)) return false;
    uint8_t xb[48];
    uint32_t xr[L];
    to_be(x, xb, len);
    rp_mont_reduce_bytes(&c.fn, xb, len, xr);
    uint8_t xrb[48];
    to_be(xr, xrb, len);
    return memcmp(xrb, r, len) == 0;
}
