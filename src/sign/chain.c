// src/sign/chain.c - certificate paths for `verify` (include/rubrapack/sign.h; RFC 5280 6, the
// parts RFC-0001 12.6 lists: signatures, validity, basicConstraints, keyUsage, EKU, critical
// extensions; path length constraints and policies are not checked and a path is refused only for
// what is checked). Revocation is never looked up: `verify` reports it as not checked.

#include "rubrapack/sign.h"

#include <string.h>

static bool sig_hash(const rp_cert_t *c, rp_hash_alg_t *alg) {
    static const uint8_t pkcs1[] = { 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01 };
    static const uint8_t ecdsa[] = { 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x04, 0x03 };        // ecdsa-with-SHA2
    rp_der_span_t s = c->sig_alg;
    rp_der_t seq, oid;
    if (!rp_der_get(&s, RP_DER_SEQUENCE, &seq)) return false;
    rp_der_span_t in = rp_der_inside(&seq);
    if (!rp_der_get(&in, RP_DER_OID, &oid)) return false;
    uint8_t last;
    if (oid.val.n == 9 && memcmp(oid.val.p, pkcs1, 8) == 0 && c->ec_curve < 0) last = (uint8_t)(oid.val.p[8] - 0x0B);
    else if (oid.val.n == 8 && memcmp(oid.val.p, ecdsa, 7) == 0) last = (uint8_t)(oid.val.p[7] - 2);
    else return false;              // SHA-1 and older are not accepted
    if (last > 2) return false;
    *alg = (rp_hash_alg_t)(RP_HASH_SHA256 + last);
    return true;
}

bool rp_cert_verify_sig(const rp_cert_t *c, rp_hash_alg_t alg, const uint8_t *digest, const uint8_t *sig, size_t len) {
    if (c->rsa) return rp_rsa_verify(c->rsa_n.p, c->rsa_n.n, c->rsa_e.p, c->rsa_e.n, alg, digest, sig, len);
    if (c->ec_curve < 0) return false;
    rp_der_span_t s = { sig, len };
    rp_der_t seq, r, t;
    if (!rp_der_get(&s, RP_DER_SEQUENCE, &seq) || s.n) return false;
    rp_der_span_t in = rp_der_inside(&seq);
    rp_der_span_t rm, sm;
    if (!rp_der_get(&in, RP_DER_INTEGER, &r) || !rp_der_get(&in, RP_DER_INTEGER, &t) || in.n || !rp_der_uint(&r, &rm) || !rp_der_uint(&t, &sm)) return false;
    size_t n = rp_ec_size((rp_ec_curve_t)c->ec_curve);
    if (rm.n > n || sm.n > n) return false;
    uint8_t rb[48] = { 0 }, sb[48] = { 0 };
    memcpy(rb + n - rm.n, rm.p, rm.n);
    memcpy(sb + n - sm.n, sm.p, sm.n);
    return rp_ecdsa_verify((rp_ec_curve_t)c->ec_curve, c->ec_x.p, c->ec_y.p, alg, digest, rb, sb);
}

bool rp_cert_signed_by(const rp_cert_t *child, const rp_cert_t *issuer) {
    rp_hash_alg_t alg;
    if (!sig_hash(child, &alg)) return false;
    if (child->issuer.n != issuer->subject.n || memcmp(child->issuer.p, issuer->subject.p, child->issuer.n) != 0) return false;
    uint8_t d[RP_HASH_MAX];
    rp_hash(alg, child->tbs.p, child->tbs.n, d);
    return rp_cert_verify_sig(issuer, alg, d, child->sig.p, child->sig.n);
}

static bool valid_at(const rp_cert_t *c, int64_t now) { return now >= c->not_before && now <= c->not_after; }

bool rp_chain_trusted(rp_der_span_t signer, rp_der_span_t certs, const rp_der_span_t *anchors, size_t anchor_count, int64_t now,
                      const char **why) {
    return rp_chain_trusted_for(signer, certs, anchors, anchor_count, now, RP_PURPOSE_CODE, why);
}

bool rp_chain_trusted_for(rp_der_span_t signer, rp_der_span_t certs, const rp_der_span_t *anchors, size_t anchor_count, int64_t now,
                          int purpose, const char **why) {
    rp_cert_t cur;
    if (!rp_cert_parse(signer.p, signer.n, &cur, why)) return false;
    if (purpose == RP_PURPOSE_CODE && (!cur.has_eku || !(cur.eku_code || cur.eku_any))) {
        *why = "the signer's certificate is not for code signing";
        return false;
    }
    if (purpose == RP_PURPOSE_TIMESTAMP && (!cur.has_eku || !cur.eku_time)) {
        *why = "the timestamp server's certificate is not for time stamping";
        return false;
    }
    for (int depth = 0; depth < 8; ++depth) {
        if (!valid_at(&cur, now)) {
            *why = depth ? "a certificate in the path is expired or not valid yet" : "the signer's certificate is expired or not valid yet";
            return false;
        }
        if (cur.unknown_critical) {
            *why = "a certificate in the path has a critical extension rubrapack does not understand";
            return false;
        }
        // An anchor that is this certificate, or that signed it, ends the path.
        for (size_t a = 0; a < anchor_count; ++a) {
            rp_cert_t anchor;
            if (!rp_cert_parse(anchors[a].p, anchors[a].n, &anchor, NULL)) continue;
            if (anchor.der.n == cur.der.n && memcmp(anchor.der.p, cur.der.p, cur.der.n) == 0) return true;
            if (rp_cert_signed_by(&cur, &anchor)) {
                if (!valid_at(&anchor, now)) {
                    *why = "the trusted certificate is expired or not valid yet";
                    return false;
                }
                return true;
            }
        }
        // Otherwise the issuer must be among the signature's certificates.
        rp_der_span_t list = certs;
        rp_der_t e;
        bool next = false;
        while (!next && rp_der_get(&list, RP_DER_SEQUENCE, &e)) {
            rp_cert_t cand;
            if (!rp_cert_parse(e.whole.p, e.whole.n, &cand, NULL) || cand.der.p == cur.der.p) continue;
            if (!rp_cert_signed_by(&cur, &cand)) continue;
            if (!cand.ca || (cand.has_ku && !cand.ku_cert_sign)) {
                *why = "a certificate in the path is signed by a certificate that is not a CA";
                return false;
            }
            cur = cand;
            next = true;
        }
        if (!next) {
            *why = "the path does not reach a trusted certificate (--trust)";
            return false;
        }
    }
    *why = "the certificate path is longer than 8";
    return false;
}
