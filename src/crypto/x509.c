// src/crypto/x509.c - X.509 certificates (include/rubrapack/pki.h; RFC 5280): proven_c_lib's
// parser (proven/cert.h), its fields given as the spans the signing code reads. The spans point
// into the original bytes.

#include "rubrapack/pki.h"

#include <string.h>

#include "proven/cert.h"
#include "proven/der.h"

static rp_der_span_t sp(proven_mem_view_t v) { return (rp_der_span_t){ v.ptr, v.size }; }

bool rp_cert_parse(const uint8_t *der, size_t len, rp_cert_t *c, const char **why) {
    memset(c, 0, sizeof *c);
    c->ec_curve = -1;
    proven_cert_t pc;
    if (proven_cert_parse((proven_mem_view_t){ der, len }, &pc) != PROVEN_OK) {
        if (why) *why = "not a well-formed DER certificate";
        return false;
    }
    c->der = sp(pc.der);
    c->tbs = sp(pc.tbs);
    c->issuer = sp(pc.issuer);
    c->subject = sp(pc.subject);
    c->spki = sp(pc.public_key_info);
    // The serial number as its whole INTEGER element, for IssuerAndSerialNumber: the element after
    // the optional [0] version at the start of tbsCertificate.
    proven_mem_view_t t = pc.tbs, inner;
    proven_der_item_t e;
    if (proven_der_expect(&t, PROVEN_DER_SEQUENCE, &inner) != PROVEN_OK || proven_der_read(&inner, &e) != PROVEN_OK ||
        (e.tag == PROVEN_DER_CONTEXT(0) && proven_der_read(&inner, &e) != PROVEN_OK) || e.tag != PROVEN_DER_INTEGER) {
        if (why) *why = "not a well-formed DER certificate";
        return false;
    }
    c->serial = sp(e.whole);
    // The signature's bits without the unused-bits byte (after tbsCertificate and signatureAlgorithm).
    proven_mem_view_t outer = pc.der, cs;
    proven_der_item_t tbs, alg;
    if (proven_der_expect(&outer, PROVEN_DER_SEQUENCE, &cs) != PROVEN_OK || proven_der_read(&cs, &tbs) != PROVEN_OK ||
        proven_der_read(&cs, &alg) != PROVEN_OK) {
        if (why) *why = "not a well-formed DER certificate";
        return false;
    }
    proven_mem_view_t bits;
    proven_u8 unused = 0;
    if (proven_der_read_bits(&cs, &bits, &unused) != PROVEN_OK || unused != 0) {
        if (why) *why = "not a well-formed DER certificate";
        return false;
    }
    c->sig = sp(bits);
    // SHA-1, MD5, PSS and anything else proven reads as a kind it does not accept are refused.
    c->sig_hash = -1;
    if ((pc.sig_kind == PROVEN_CERT_SIG_RSA_PKCS1 || pc.sig_kind == PROVEN_CERT_SIG_ECDSA) && pc.sig_hash >= PROVEN_HMAC_SHA256 &&
        pc.sig_hash <= PROVEN_HMAC_SHA512) {
        c->sig_hash = pc.sig_hash == PROVEN_HMAC_SHA256 ? RP_HASH_SHA256 : pc.sig_hash == PROVEN_HMAC_SHA384 ? RP_HASH_SHA384 : RP_HASH_SHA512;
        c->sig_rsa = pc.sig_kind == PROVEN_CERT_SIG_RSA_PKCS1;
    }
    c->not_before = pc.not_before;
    c->not_after = pc.not_after;
    if (pc.key_kind == PROVEN_CERT_KEY_RSA) {
        c->rsa = true;
        c->rsa_n = sp(pc.rsa_n);
        c->rsa_e = sp(pc.rsa_e);
    } else if (pc.key_kind == PROVEN_CERT_KEY_EC_P256 || pc.key_kind == PROVEN_CERT_KEY_EC_P384) {
        size_t n = pc.key_kind == PROVEN_CERT_KEY_EC_P256 ? 32 : 48;
        if (pc.key.size == 1 + 2 * n && pc.key.ptr[0] == 4) {
            c->ec_curve = n == 32 ? RP_EC_P256 : RP_EC_P384;
            c->ec_x = (rp_der_span_t){ pc.key.ptr + 1, n };
            c->ec_y = (rp_der_span_t){ pc.key.ptr + 1 + n, n };
        }
    }
    c->ski = sp(pc.subject_key_id);
    c->has_ku = pc.has_key_usage;
    c->ku_sign = pc.key_usage & PROVEN_CERT_KU_DIGITAL_SIGNATURE;
    c->ku_cert_sign = pc.key_usage & PROVEN_CERT_KU_KEY_CERT_SIGN;
    c->has_eku = pc.has_ext_key_usage;
    c->eku_code = pc.ext_key_usage & PROVEN_CERT_EKU_CODE_SIGNING;
    c->eku_time = pc.ext_key_usage & PROVEN_CERT_EKU_TIME_STAMPING;
    c->eku_server = pc.ext_key_usage & PROVEN_CERT_EKU_SERVER_AUTH;
    c->eku_any = pc.ext_key_usage & PROVEN_CERT_EKU_ANY;
    c->san = sp(pc.alt_names);
    c->ca = pc.is_ca;
    // proven reads name constraints but rubrapack's path check does not apply them, so a
    // certificate that has them is treated as one with a critical extension not understood.
    c->unknown_critical = pc.unknown_critical || pc.name_constraints.size;
    return true;
}
