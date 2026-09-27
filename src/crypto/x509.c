// src/crypto/x509.c - X.509 certificate parsing (include/rubrapack/pki.h; RFC 5280).
//
// Written on the approach of lowent_lang v1.3.0 lib/x509.low (same author, MIT): the fields are
// spans into the original bytes, found with the DER cursor. Extended here with validity, key
// usage, extended key usage, basic constraints and the critical-extension check that code
// signing needs.

#include "rubrapack/pki.h"

#include <string.h>

static const uint8_t OID_RSA[] = { 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x01 };
static const uint8_t OID_EC[] = { 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x02, 0x01 };
static const uint8_t OID_P256[] = { 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x03, 0x01, 0x07 };
static const uint8_t OID_P384[] = { 0x2B, 0x81, 0x04, 0x00, 0x22 };
static const uint8_t OID_KU[] = { 0x55, 0x1D, 0x0F }, OID_BC[] = { 0x55, 0x1D, 0x13 }, OID_EKU[] = { 0x55, 0x1D, 0x25 };
static const uint8_t OID_SAN[] = { 0x55, 0x1D, 0x11 }, OID_SKI[] = { 0x55, 0x1D, 0x0E }, OID_AKI[] = { 0x55, 0x1D, 0x23 };
static const uint8_t OID_CODE[] = { 0x2B, 0x06, 0x01, 0x05, 0x05, 0x07, 0x03, 0x03 };
static const uint8_t OID_TIME[] = { 0x2B, 0x06, 0x01, 0x05, 0x05, 0x07, 0x03, 0x08 };
static const uint8_t OID_SERVER[] = { 0x2B, 0x06, 0x01, 0x05, 0x05, 0x07, 0x03, 0x01 };
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
            c->eku_server |= rp_der_oid_is(&o, OID_SERVER, sizeof OID_SERVER);
            c->eku_any |= rp_der_oid_is(&o, OID_ANY_EKU, sizeof OID_ANY_EKU);
        }
        if (list.n) FAIL("a malformed extended key usage");
    } else if (rp_der_oid_is(&id, OID_SAN, sizeof OID_SAN)) {
        if (!rp_der_get(&v, RP_DER_SEQUENCE, &x) || v.n) FAIL("a malformed subject alternative name");
        c->san = rp_der_inside(&x);
        rp_der_span_t list = c->san;
        rp_der_t n;
        while (list.n) {
            if (!rp_der_read(&list, &n)) FAIL("a malformed subject alternative name");
        }
    } else if (rp_der_oid_is(&id, OID_SKI, sizeof OID_SKI)) {
        if (!rp_der_get(&v, RP_DER_OCTET_STRING, &x)) FAIL("a malformed subject key identifier");
        c->ski = x.val;
    } else if (critical && !rp_der_oid_is(&id, OID_SAN, sizeof OID_SAN) && !rp_der_oid_is(&id, OID_SKI, sizeof OID_SKI) &&
               !rp_der_oid_is(&id, OID_AKI, sizeof OID_AKI)) {
        c->unknown_critical = true;
    }
    return true;
}

bool rp_cert_parse(const uint8_t *der, size_t len, rp_cert_t *c, const char **why) {
    memset(c, 0, sizeof *c);
    c->ec_curve = -1;
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
    } else if (rp_der_oid_is(&oid, OID_EC, sizeof OID_EC)) {
        rp_der_t curve;
        if (!rp_der_get(&as, RP_DER_OID, &curve)) FAIL("an EC key without a named curve");
        size_t n = rp_der_oid_is(&curve, OID_P256, sizeof OID_P256) ? 32 : rp_der_oid_is(&curve, OID_P384, sizeof OID_P384) ? 48 : 0;
        // BIT STRING: no unused bits, then 04 || X || Y.
        if (n && bits.val.n == 2 + 2 * n && bits.val.p[0] == 0 && bits.val.p[1] == 4) {
            c->ec_curve = n == 32 ? RP_EC_P256 : RP_EC_P384;
            c->ec_x = (rp_der_span_t){ bits.val.p + 2, n };
            c->ec_y = (rp_der_span_t){ bits.val.p + 2 + n, n };
        }
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

// ---- host names (RFC 6125) ----------------------------------------------------------------------

static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }

// ASCII case-insensitive equality of a[0..n) and b[0..n).
static bool ieq(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if (lower(a[i]) != lower(b[i])) return false;
    }
    return true;
}

static bool dns_match(const char *pat, size_t pl, const char *host, size_t hl) {
    if (pl && pat[pl - 1] == '.') --pl;
    if (hl && host[hl - 1] == '.') --hl;
    if (pl == 0 || hl == 0) return false;
    for (size_t i = 0; i < pl; ++i) {
        if ((unsigned char)pat[i] <= 0x20 || (unsigned char)pat[i] >= 0x7F) return false;
    }
    if (pl >= 2 && pat[0] == '*' && pat[1] == '.') {
        const char *rest = pat + 1;                        // ".example.com"
        size_t rl = pl - 1;
        if (memchr(rest + 1, '.', rl - 1) == NULL) return false;    // "*.com": too wide
        if (memchr(pat + 2, '*', pl - 2)) return false;
        const char *dot = memchr(host, '.', hl);
        if (dot == NULL || dot == host) return false;      // exactly one non-empty label
        size_t tail = hl - (size_t)(dot - host);
        return tail == rl && ieq(dot, rest, rl);
    }
    if (memchr(pat, '*', pl)) return false;                // no partial wildcards
    return pl == hl && ieq(pat, host, pl);
}

size_t rp_ip_parse(const char *s, uint8_t out[16]) {
    // IPv4: four decimal parts 0-255, no leading zeros beyond a single 0.
    {
        const char *p = s;
        int parts = 0;
        uint8_t v4[4];
        bool ok = true;
        while (ok && parts < 4) {
            if (*p < '0' || *p > '9') {
                ok = false;
                break;
            }
            unsigned v = 0, digits = 0;
            const char *start = p;
            while (*p >= '0' && *p <= '9' && digits < 4) v = v * 10 + (unsigned)(*p++ - '0'), ++digits;
            if (digits == 0 || digits > 3 || v > 255 || (digits > 1 && *start == '0')) ok = false;
            v4[parts++] = (uint8_t)v;
            if (parts < 4) {
                if (*p != '.') ok = false;
                else ++p;
            }
        }
        if (ok && parts == 4 && *p == 0) {
            memcpy(out, v4, 4);
            return 4;
        }
    }
    // IPv6: up to 8 groups of 1-4 hex digits, one "::" for a run of zero groups (no embedded IPv4).
    uint16_t g[8];
    int n = 0, gap = -1;
    const char *p = s;
    if (p[0] == ':' && p[1] == ':') {
        gap = 0;
        p += 2;
    } else if (p[0] == ':') {
        return 0;
    }
    while (*p) {
        if (n == 8) return 0;
        unsigned v = 0, digits = 0;
        while (digits < 5) {
            char c = lower(*p);
            unsigned d = c >= '0' && c <= '9' ? (unsigned)(c - '0') : c >= 'a' && c <= 'f' ? (unsigned)(c - 'a' + 10) : 16;
            if (d == 16) break;
            v = v * 16 + d;
            ++digits;
            ++p;
        }
        if (digits == 0 || digits > 4) return 0;
        g[n++] = (uint16_t)v;
        if (*p == 0) break;
        if (*p != ':') return 0;
        ++p;
        if (*p == ':') {
            if (gap >= 0) return 0;
            gap = n;
            ++p;
            if (*p == 0) break;
        } else if (*p == 0) {
            return 0;                                      // a trailing single ':'
        }
    }
    if (gap < 0 ? n != 8 : n > 7) return 0;
    memset(out, 0, 16);
    int tail = gap < 0 ? 0 : n - gap, head = gap < 0 ? n : gap;
    for (int i = 0; i < head; ++i) out[2 * i] = (uint8_t)(g[i] >> 8), out[2 * i + 1] = (uint8_t)g[i];
    for (int i = 0; i < tail; ++i) {
        int at = 8 - tail + i;
        out[2 * at] = (uint8_t)(g[gap + i] >> 8);
        out[2 * at + 1] = (uint8_t)g[gap + i];
    }
    return 16;
}

bool rp_cert_names_host(const rp_cert_t *c, const char *host) {
    uint8_t ip[16];
    size_t il = rp_ip_parse(host, ip);
    rp_der_span_t list = c->san;
    rp_der_t n;
    while (rp_der_read(&list, &n)) {
        if (il == 0 && n.tag == 0x82 && dns_match((const char *)n.val.p, n.val.n, host, strlen(host))) return true;   // [2] dNSName
        if (il && n.tag == 0x87 && n.val.n == il && memcmp(n.val.p, ip, il) == 0) return true;                        // [7] iPAddress
    }
    return false;
}
