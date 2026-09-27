// src/crypto/rsa.c - RSA PKCS#1 v1.5 signatures (RFC 8017 8.2, 9.2) and RSASSA-PSS verification
// (RFC 8017 8.1.2, 9.1.2) (include/rubrapack/crypto.h).
//
// Signing uses the CRT with base blinding on each prime: for a random r < p the message is
// multiplied by r^e, raised to dP (giving m1 r, since e dP = 1 mod p - 1), and divided by r again
// with r^-1 = r^(p-2) (Fermat). Every exponentiation that sees a secret is rp_mont_exp_ct. The
// finished signature is raised to e and compared with the encoded message before it is returned,
// so a wrong key component or a computation fault never leaves as a signature.
// Verification (public values only) is new code; lowent_lang v1.3.0 lib/rsa.low has the same
// steps for its TLS use.

#include "rubrapack/crypto.h"

#include <string.h>

#include "proven/random.h"

static const uint8_t *digest_info(rp_hash_alg_t alg, size_t *len) {
    static const uint8_t s256[] = { 0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20 };
    static const uint8_t s384[] = { 0x30, 0x41, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x02, 0x05, 0x00, 0x04, 0x30 };
    static const uint8_t s512[] = { 0x30, 0x51, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x03, 0x05, 0x00, 0x04, 0x40 };
    *len = sizeof s256;
    return alg == RP_HASH_SHA256 ? s256 : alg == RP_HASH_SHA384 ? s384 : s512;
}

static void strip(const uint8_t **p, size_t *n) {
    while (*n && (*p)[0] == 0) {
        ++*p;
        --*n;
    }
}

// EM = 00 01 FF..FF 00 DigestInfo digest, `k` bytes.
static bool encode(rp_hash_alg_t alg, const uint8_t *digest, uint8_t *em, size_t k) {
    size_t pl, hl = rp_hash_size(alg);
    const uint8_t *prefix = digest_info(alg, &pl);
    if (k < pl + hl + 11) return false;
    em[0] = 0;
    em[1] = 1;
    memset(em + 2, 0xFF, k - pl - hl - 3);
    em[k - pl - hl - 1] = 0;
    memcpy(em + k - pl - hl, prefix, pl);
    memcpy(em + k - hl, digest, hl);
    return true;
}

static void to_be(const uint32_t *v, size_t limbs, uint8_t *out, size_t len) {
    for (size_t i = 0; i < len; ++i) out[len - 1 - i] = i / 4 < limbs ? (uint8_t)(v[i / 4] >> (8 * (i % 4))) : 0;
}

static void from_be(uint32_t *v, size_t limbs, const uint8_t *b, size_t len) {
    memset(v, 0, limbs * sizeof *v);
    for (size_t i = 0; i < len && i < 4 * limbs; ++i) v[i / 4] |= (uint32_t)b[len - 1 - i] << (8 * (i % 4));
}

// r = a - b mod p for a, b < p, without a data-dependent branch.
static void sub_mod(const rp_mont_t *m, uint32_t *r, const uint32_t *a, const uint32_t *b) {
    uint64_t borrow = 0;
    for (size_t j = 0; j < m->k; ++j) {
        uint64_t d = (uint64_t)a[j] - b[j] - borrow;
        r[j] = (uint32_t)d;
        borrow = (d >> 63) & 1;
    }
    uint32_t mask = (uint32_t)0 - (uint32_t)borrow;
    uint64_t c = 0;
    for (size_t j = 0; j < m->k; ++j) {
        c += (uint64_t)r[j] + (m->n[j] & mask);
        r[j] = (uint32_t)c;
        c >>= 32;
    }
}

// m = c^d mod p with blinding (see the file comment). c is the message reduced mod p.
static proven_err_t crt_half(const rp_mont_t *mp, const uint8_t *p, size_t p_len, const uint8_t *e, size_t e_len,
                             const uint8_t *d, size_t d_len, const uint32_t *c, uint32_t *out) {
    uint8_t rb[4 * RP_BN_LIMBS + 8], pm2[4 * RP_BN_LIMBS];
    uint32_t r[RP_BN_LIMBS], re[RP_BN_LIMBS], cb[RP_BN_LIMBS], mb[RP_BN_LIMBS], rinv[RP_BN_LIMBS];
    do {
        if (!proven_random_bytes(rb, p_len + 8)) return PROVEN_ERR_IO;
        rp_mont_reduce_bytes(mp, rb, p_len + 8, r);
        uint32_t any = 0;
        for (size_t j = 0; j < mp->k; ++j) any |= r[j];
        if (any) break;
    } while (true);
    rp_mont_exp_pub(mp, re, r, e, e_len);          // r^e
    rp_mont_mulmod(mp, cb, c, re);                  // c r^e
    // The exponent at the prime's full width: its own length (leading zero bytes) stays hidden.
    uint8_t dpad[4 * RP_BN_LIMBS] = { 0 };
    if (d_len > p_len) {
        for (size_t i = 0; i + p_len < d_len; ++i) {
            if (d[i]) return PROVEN_ERR_INVALID_ARG;        // d >= p: not a CRT exponent
        }
        d += d_len - p_len;
        d_len = p_len;
    }
    memcpy(dpad + p_len - d_len, d, d_len);
    rp_mont_exp_ct(mp, mb, cb, dpad, p_len);        // c^d r
    rp_wipe(dpad, sizeof dpad);
    memcpy(pm2, p, p_len);                          // p - 2 (p is odd and > 2)
    for (size_t i = p_len, borrow = 2; i-- && borrow;) {
        unsigned v = pm2[i];
        pm2[i] = (uint8_t)(v - borrow);
        borrow = v < borrow ? 1 : 0;
    }
    rp_mont_exp_ct(mp, rinv, r, pm2, p_len);        // r^-1
    rp_mont_mulmod(mp, out, mb, rinv);              // c^d
    rp_wipe(rb, sizeof rb);
    rp_wipe(pm2, sizeof pm2);
    rp_wipe(r, sizeof r);
    rp_wipe(re, sizeof re);
    rp_wipe(cb, sizeof cb);
    rp_wipe(mb, sizeof mb);
    rp_wipe(rinv, sizeof rinv);
    return PROVEN_OK;
}

proven_err_t rp_rsa_sign(const rp_rsa_key_t *key, rp_hash_alg_t alg, const uint8_t *digest, uint8_t *sig, size_t *sig_len) {
    const uint8_t *n = key->n, *p = key->p, *q = key->q, *e = key->e;
    size_t nl = key->n_len, pl = key->p_len, ql = key->q_len, el = key->e_len;
    strip(&n, &nl);
    strip(&p, &pl);
    strip(&q, &ql);
    strip(&e, &el);
    if (!nl || !pl || !ql || !el || !key->dp || !key->dq || !key->qinv || nl > 4 * RP_BN_LIMBS || nl < 256) return PROVEN_ERR_INVALID_ARG;
    rp_mont_t mn, mp, mq;
    if (!rp_mont_init(&mn, n, nl) || !rp_mont_init(&mp, p, pl) || !rp_mont_init(&mq, q, ql)) return PROVEN_ERR_INVALID_ARG;
    // n = p q, or the components do not belong together.
    uint32_t prod[2 * RP_BN_LIMBS] = { 0 }, pv[RP_BN_LIMBS], qv[RP_BN_LIMBS];
    from_be(pv, mp.k, p, pl);
    from_be(qv, mq.k, q, ql);
    for (size_t i = 0; i < mq.k; ++i) {
        uint64_t c = 0;
        for (size_t j = 0; j < mp.k; ++j) {
            c += (uint64_t)prod[i + j] + (uint64_t)pv[j] * qv[i];
            prod[i + j] = (uint32_t)c;
            c >>= 32;
        }
        prod[i + mp.k] = (uint32_t)c;
    }
    uint8_t nb[4 * 2 * RP_BN_LIMBS];
    size_t wide = 4 * (mp.k + mq.k);            // p q written at full width, n padded to match
    to_be(prod, mp.k + mq.k, nb, wide);
    bool zeros = true;
    for (size_t i = 0; i + nl < wide; ++i) zeros &= nb[i] == 0;
    if (wide < nl || !zeros || memcmp(nb + wide - nl, n, nl) != 0) return PROVEN_ERR_INVALID_ARG;

    uint8_t em[4 * RP_BN_LIMBS];
    if (!encode(alg, digest, em, nl)) return PROVEN_ERR_INVALID_ARG;
    uint32_t cp[RP_BN_LIMBS], cq[RP_BN_LIMBS], m1[RP_BN_LIMBS], m2[RP_BN_LIMBS], m2p[RP_BN_LIMBS], qinv[RP_BN_LIMBS],
        h[RP_BN_LIMBS], diff[RP_BN_LIMBS];
    rp_mont_reduce_bytes(&mp, em, nl, cp);
    rp_mont_reduce_bytes(&mq, em, nl, cq);
    proven_err_t err = crt_half(&mp, p, pl, e, el, key->dp, key->dp_len, cp, m1);
    if (err == PROVEN_OK) err = crt_half(&mq, q, ql, e, el, key->dq, key->dq_len, cq, m2);
    if (err != PROVEN_OK) return err;
    // h = qinv (m1 - m2) mod p; s = m2 + h q.
    uint8_t m2b[4 * RP_BN_LIMBS];
    to_be(m2, mq.k, m2b, 4 * mq.k);
    rp_mont_reduce_bytes(&mp, m2b, 4 * mq.k, m2p);
    rp_mont_reduce_bytes(&mp, key->qinv, key->qinv_len, qinv);
    sub_mod(&mp, diff, m1, m2p);
    rp_mont_mulmod(&mp, h, qinv, diff);
    uint32_t s[2 * RP_BN_LIMBS] = { 0 };
    for (size_t i = 0; i < mq.k; ++i) {
        uint64_t c = 0;
        for (size_t j = 0; j < mp.k; ++j) {
            c += (uint64_t)s[i + j] + (uint64_t)h[j] * qv[i];
            s[i + j] = (uint32_t)c;
            c >>= 32;
        }
        s[i + mp.k] = (uint32_t)c;
    }
    uint64_t c = 0;
    for (size_t j = 0; j < mp.k + mq.k; ++j) {
        c += (uint64_t)s[j] + (j < mq.k ? m2[j] : 0);
        s[j] = (uint32_t)c;
        c >>= 32;
    }
    to_be(s, mp.k + mq.k, sig, nl);
    *sig_len = nl;
    rp_wipe(cp, sizeof cp);
    rp_wipe(cq, sizeof cq);
    rp_wipe(m1, sizeof m1);
    rp_wipe(m2, sizeof m2);
    rp_wipe(m2p, sizeof m2p);
    rp_wipe(m2b, sizeof m2b);
    rp_wipe(h, sizeof h);
    rp_wipe(diff, sizeof diff);
    rp_wipe(s, sizeof s);
    rp_wipe(pv, sizeof pv);
    rp_wipe(qv, sizeof qv);
    if (!rp_rsa_verify(n, nl, e, el, alg, digest, sig, nl)) {
        rp_wipe(sig, nl);
        return PROVEN_ERR_INVALID_STATE;
    }
    return PROVEN_OK;
}

bool rp_rsa_verify(const uint8_t *n, size_t n_len, const uint8_t *e, size_t e_len, rp_hash_alg_t alg, const uint8_t *digest,
                   const uint8_t *sig, size_t sig_len) {
    strip(&n, &n_len);
    strip(&e, &e_len);
    rp_mont_t mn;
    if (sig_len != n_len || !e_len || !rp_mont_init(&mn, n, n_len)) return false;
    if (memcmp(sig, n, n_len) >= 0) return false;       // s < n (same length, big-endian)
    uint32_t sv[RP_BN_LIMBS], mv[RP_BN_LIMBS];
    from_be(sv, mn.k, sig, sig_len);
    rp_mont_exp_pub(&mn, mv, sv, e, e_len);
    uint8_t em[4 * RP_BN_LIMBS], got[4 * RP_BN_LIMBS];
    if (!encode(alg, digest, em, n_len)) return false;
    to_be(mv, mn.k, got, n_len);
    return rp_ct_equal(em, got, n_len);
}

// MGF1 (RFC 8017 B.2.1) XORed into `db`.
static void mgf1_xor(rp_hash_alg_t alg, const uint8_t *seed, size_t seed_len, uint8_t *db, size_t len) {
    uint8_t h[RP_HASH_MAX];
    size_t hl = rp_hash_size(alg);
    for (uint32_t c = 0, off = 0; off < len; ++c) {
        uint8_t ctr[4] = { (uint8_t)(c >> 24), (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c };
        rp_hash_t t;
        rp_hash_init(&t, alg);
        rp_hash_update(&t, seed, seed_len);
        rp_hash_update(&t, ctr, 4);
        rp_hash_final(&t, h);
        for (size_t i = 0; i < hl && off < len; ++i) db[off++] ^= h[i];
    }
}

bool rp_rsa_pss_verify(const uint8_t *n, size_t n_len, const uint8_t *e, size_t e_len, rp_hash_alg_t alg, const uint8_t *digest,
                       const uint8_t *sig, size_t sig_len, size_t salt_len) {
    strip(&n, &n_len);
    strip(&e, &e_len);
    rp_mont_t mn;
    if (sig_len != n_len || !e_len || !rp_mont_init(&mn, n, n_len)) return false;
    if (memcmp(sig, n, n_len) >= 0) return false;
    uint32_t sv[RP_BN_LIMBS], mv[RP_BN_LIMBS];
    from_be(sv, mn.k, sig, sig_len);
    rp_mont_exp_pub(&mn, mv, sv, e, e_len);
    uint8_t m[4 * RP_BN_LIMBS];
    to_be(mv, mn.k, m, n_len);
    // emBits = modBits - 1; EM is the last emLen bytes of m, whose leading bytes must be zero.
    size_t mod_bits = 8 * n_len;
    for (uint8_t top = n[0]; !(top & 0x80); top = (uint8_t)(top << 1)) --mod_bits;
    size_t em_bits = mod_bits - 1, em_len = (em_bits + 7) / 8, hl = rp_hash_size(alg);
    for (size_t i = 0; i < n_len - em_len; ++i) {
        if (m[i]) return false;
    }
    uint8_t *em = m + (n_len - em_len);
    if (em_len < hl + salt_len + 2 || em[em_len - 1] != 0xBC) return false;
    size_t db_len = em_len - hl - 1;
    uint8_t top_mask = (uint8_t)(0xFF >> (8 * em_len - em_bits));
    if (em[0] & (uint8_t)~top_mask) return false;
    const uint8_t *hash = em + db_len;
    mgf1_xor(alg, hash, hl, em, db_len);            // em[0..db_len) becomes DB
    em[0] &= top_mask;
    for (size_t i = 0; i < db_len - salt_len - 1; ++i) {
        if (em[i]) return false;
    }
    if (em[db_len - salt_len - 1] != 0x01) return false;
    uint8_t h2[RP_HASH_MAX], zeros[8] = { 0 };
    rp_hash_t t;
    rp_hash_init(&t, alg);
    rp_hash_update(&t, zeros, 8);
    rp_hash_update(&t, digest, hl);
    rp_hash_update(&t, em + db_len - salt_len, salt_len);
    rp_hash_final(&t, h2);
    return memcmp(h2, hash, hl) == 0;
}
