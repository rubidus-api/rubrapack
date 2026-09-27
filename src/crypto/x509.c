// src/crypto/x509.c - X.509 certificate parsing (include/rubrapack/pki.h; RFC 5280).
//
// Written on the approach of lowent_lang v1.3.0 lib/x509.low (same author, MIT): the fields are
// spans into the original bytes, found with the DER cursor. Extended here with validity, key
// usage, extended key usage, basic constraints and the critical-extension check that code
// signing needs.

#include "rubrapack/pki.h"

#include <string.h>

static const uint8_t OID_RSA[] = { 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x01 };
static const uint8_t OID_KU[] = { 0x55, 0x1D, 0x0F }, OID_BC[] = { 0x55, 0x1D, 0x13 }, OID_EKU[] = { 0x55, 0x1D, 0x25 };
static const uint8_t OID_SAN[] = { 0x55, 0x1D, 0x11 }, OID_SKI[] = { 0x55, 0x1D, 0x0E }, OID_AKI[] = { 0x55, 0x1D, 0x23 };
static const uint8_t OID_CODE[] = { 0x2B, 0x06, 0x01, 0x05, 0x05, 0x07, 0x03, 0x03 };
static const uint8_t OID_TIME[] = { 0x2B, 0x06, 0x01, 0x05, 0x05, 0x07, 0x03, 0x08 };
static const uint8_t OID_ANY_EKU[] = { 0x55, 0x1D, 0x25, 0x00 };

// Days from 1970-01-01 to a civil date (proleptic Gregorian).
static int64_t days_from_civil(int64_t y, int m, int d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static bool digits(const uint8_t *p, size_t n, int *v) {
    *v = 0;
    for (size_t i = 0; i < n; ++i) {
        if (p[i] < '0' || p[i] > '9') return false;
        *v = *v * 10 + (p[i] - '0');
    }
    return true;
}

static bool parse_time(const rp_der_t *e, int64_t *out) {
    const uint8_t *p = e->val.p;
    int y, mo, d, h, mi, s;
    size_t off;
    if (e->tag == RP_DER_UTCTIME && e->val.n == 13) {
        if (!digits(p, 2, &y)) return false;
        y += y < 50 ? 2000 : 1900;
        off = 2;
    } else if (e->tag == RP_DER_GENTIME && e->val.n == 15) {
        if (!digits(p, 4, &y)) return false;
        off = 4;
    } else {
        return false;
    }
    if (p[e->val.n - 1] != 'Z' || !digits(p + off, 2, &mo) || !digits(p + off + 2, 2, &d) || !digits(p + off + 4, 2, &h) ||
        !digits(p + off + 6, 2, &mi) || !digits(p + off + 8, 2, &s) || mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 ||
        mi > 59 || s > 60) {
        return false;
    }
    *out = days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + s;
    return true;
}

#define FAIL(msg) do { *why = (msg); return false; } while (0)

static bool parse_ext(const rp_der_t *ext, rp_cert_t *c, const char **why) {
    rp_der_span_t s = rp_der_inside(ext), v;
    rp_der_t id, crit, val, x;
    bool critical = false;
    if (!rp_der_get(&s, RP_DER_OID, &id)) FAIL("an extension without an identifier");
    if (rp_der_get(&s, RP_DER_BOOLEAN, &crit)) critical = crit.val.n == 1 && crit.val.p[0] != 0;
    if (!rp_der_get(&s, RP_DER_OCTET_STRING, &val) || s.n) FAIL("a malformed extension");
    v = val.val;
    if (rp_der_oid_is(&id, OID_KU, sizeof OID_KU)) {
        if (!rp_der_get(&v, RP_DER_BIT_STRING, &x) || x.val.n < 2) FAIL("a malformed key usage");
        c->has_ku = true;
        c->ku_sign = x.val.p[1] & 0x80;
        c->ku_cert_sign = x.val.p[1] & 0x04;
    } else if (rp_der_oid_is(&id, OID_BC, sizeof OID_BC)) {
        if (!rp_der_get(&v, RP_DER_SEQUENCE, &x)) FAIL("malformed basic constraints");
        rp_der_span_t b = rp_der_inside(&x);
        rp_der_t ca;
        if (rp_der_get(&b, RP_DER_BOOLEAN, &ca)) c->ca = ca.val.n == 1 && ca.val.p[0] != 0;
    } else if (rp_der_oid_is(&id, OID_EKU, sizeof OID_EKU)) {
        if (!rp_der_get(&v, RP_DER_SEQUENCE, &x)) FAIL("a malformed extended key usage");
        c->has_eku = true;
        rp_der_span_t list = rp_der_inside(&x);
        rp_der_t o;
        while (rp_der_get(&list, RP_DER_OID, &o)) {
            c->eku_code |= rp_der_oid_is(&o, OID_CODE, sizeof OID_CODE);
            c->eku_time |= rp_der_oid_is(&o, OID_TIME, sizeof OID_TIME);
            c->eku_any |= rp_der_oid_is(&o, OID_ANY_EKU, sizeof OID_ANY_EKU);
        }
        if (list.n) FAIL("a malformed extended key usage");
    } else if (critical && !rp_der_oid_is(&id, OID_SAN, sizeof OID_SAN) && !rp_der_oid_is(&id, OID_SKI, sizeof OID_SKI) &&
               !rp_der_oid_is(&id, OID_AKI, sizeof OID_AKI)) {
        c->unknown_critical = true;
    }
    return true;
}

bool rp_cert_parse(const uint8_t *der, size_t len, rp_cert_t *c, const char **why) {
    memset(c, 0, sizeof *c);
    const char *dummy;
    if (why == NULL) why = &dummy;
    rp_der_span_t s = { der, len };
    rp_der_t cert, tbs, e;
    if (!rp_der_get(&s, RP_DER_SEQUENCE, &cert) || s.n) FAIL("not a DER certificate");
    c->der = cert.whole;
    rp_der_span_t cs = rp_der_inside(&cert);
    if (!rp_der_get(&cs, RP_DER_SEQUENCE, &tbs)) FAIL("no tbsCertificate");
    c->tbs = tbs.whole;
    if (!rp_der_get(&cs, RP_DER_SEQUENCE, &e)) FAIL("no signature algorithm");
    c->sig_alg = e.whole;
    if (!rp_der_get(&cs, RP_DER_BIT_STRING, &e) || e.val.n < 1 || e.val.p[0] != 0 || cs.n) FAIL("a malformed signature");
    c->sig = (rp_der_span_t){ e.val.p + 1, e.val.n - 1 };

    rp_der_span_t t = rp_der_inside(&tbs);
    uint64_t version = 0;
    if (rp_der_get(&t, RP_DER_CTX0, &e)) {
        rp_der_span_t vs = rp_der_inside(&e);
        rp_der_t v;
        if (!rp_der_get(&vs, RP_DER_INTEGER, &v) || !rp_der_small(&v, &version) || version > 2) FAIL("an unknown certificate version");
    }
    if (!rp_der_get(&t, RP_DER_INTEGER, &e)) FAIL("no serial number");
    c->serial = e.whole;
    if (!rp_der_get(&t, RP_DER_SEQUENCE, &e)) FAIL("no inner signature algorithm");
    if (!rp_der_get(&t, RP_DER_SEQUENCE, &e)) FAIL("no issuer");
    c->issuer = e.whole;
    if (!rp_der_get(&t, RP_DER_SEQUENCE, &e)) FAIL("no validity");
    rp_der_span_t vs = rp_der_inside(&e);
    rp_der_t nb, na;
    if (!rp_der_read(&vs, &nb) || !rp_der_read(&vs, &na) || !parse_time(&nb, &c->not_before) || !parse_time(&na, &c->not_after)) {
        FAIL("a malformed validity");
    }
    if (!rp_der_get(&t, RP_DER_SEQUENCE, &e)) FAIL("no subject");
    c->subject = e.whole;
    if (!rp_der_get(&t, RP_DER_SEQUENCE, &e)) FAIL("no public key");
    c->spki = e.whole;
    rp_der_span_t ks = rp_der_inside(&e);
    rp_der_t alg, bits, oid;
    if (!rp_der_get(&ks, RP_DER_SEQUENCE, &alg) || !rp_der_get(&ks, RP_DER_BIT_STRING, &bits)) FAIL("a malformed public key");
    rp_der_span_t as = rp_der_inside(&alg);
    if (!rp_der_get(&as, RP_DER_OID, &oid)) FAIL("a public key without an algorithm");
    if (rp_der_oid_is(&oid, OID_RSA, sizeof OID_RSA)) {
        if (bits.val.n < 1 || bits.val.p[0] != 0) FAIL("a malformed RSA key");
        rp_der_span_t kb = { bits.val.p + 1, bits.val.n - 1 };
        rp_der_t seq, n, ex;
        if (!rp_der_get(&kb, RP_DER_SEQUENCE, &seq)) FAIL("a malformed RSA key");
        rp_der_span_t kk = rp_der_inside(&seq);
        if (!rp_der_get(&kk, RP_DER_INTEGER, &n) || !rp_der_get(&kk, RP_DER_INTEGER, &ex) || !rp_der_uint(&n, &c->rsa_n) ||
            !rp_der_uint(&ex, &c->rsa_e)) {
            FAIL("a malformed RSA key");
        }
        c->rsa = true;
    }
    // Optional issuerUniqueID [1], subjectUniqueID [2], then extensions [3].
    uint8_t tag;
    while (rp_der_peek(&t, &tag) && (tag == 0x81 || tag == 0xA1 || tag == 0x82 || tag == 0xA2)) {
        if (!rp_der_read(&t, &e)) FAIL("a malformed unique identifier");
    }
    if (rp_der_get(&t, RP_DER_CTX3, &e)) {
        rp_der_span_t xs = rp_der_inside(&e);
        rp_der_t list, ext;
        if (!rp_der_get(&xs, RP_DER_SEQUENCE, &list) || xs.n) FAIL("malformed extensions");
        rp_der_span_t ls = rp_der_inside(&list);
        while (rp_der_get(&ls, RP_DER_SEQUENCE, &ext)) {
            if (!parse_ext(&ext, c, why)) return false;
        }
        if (ls.n) FAIL("malformed extensions");
    }
    if (t.n) FAIL("unexpected data in the certificate");
    return true;
}
