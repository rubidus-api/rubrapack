// src/sign/tsp.c - RFC 3161 time-stamp requests and responses (include/rubrapack/sign.h; RFC-0008).
// RFC 3161, RFC 5816 (ESSCertIDv2 is not required here), RFC 5652 for the token's SignedData.

#include "rubrapack/mem.h"
#include "rubrapack/net.h"
#include "rubrapack/sign.h"

#include <string.h>

#include "proven/random.h"

#define OID(name, ...) static const uint8_t name[] = { __VA_ARGS__ }
OID(O_SIGNED_DATA, 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x07, 0x02);
OID(O_TSTINFO, 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x09, 0x10, 0x01, 0x04);
OID(O_CONTENT_TYPE, 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x09, 0x03);
OID(O_MESSAGE_DIGEST, 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x09, 0x04);
OID(O_RSA_PSS, 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x0A);
OID(O_SHA256, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01);
OID(O_SHA384, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x02);
OID(O_SHA512, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x03);

static const uint8_t *hash_oid(rp_hash_alg_t alg) { return alg == RP_HASH_SHA256 ? O_SHA256 : alg == RP_HASH_SHA384 ? O_SHA384 : O_SHA512; }

static bool alg_of(const rp_der_t *algid, rp_hash_alg_t *alg) {
    rp_der_span_t s = rp_der_inside(algid);
    rp_der_t oid;
    if (!rp_der_get(&s, RP_DER_OID, &oid)) return false;
    if (rp_der_oid_is(&oid, O_SHA256, 9)) *alg = RP_HASH_SHA256;
    else if (rp_der_oid_is(&oid, O_SHA384, 9)) *alg = RP_HASH_SHA384;
    else if (rp_der_oid_is(&oid, O_SHA512, 9)) *alg = RP_HASH_SHA512;
    else return false;
    return true;
}

proven_err_t rp_tsp_request(proven_allocator_t alloc, rp_hash_alg_t alg, const uint8_t *digest, uint8_t nonce[8], uint8_t **out, size_t *len) {
    nonce[0] &= 0x7F;
    if (nonce[0] == 0) nonce[0] = 1;                // eight significant bytes, positive
    rp_buf_t req = rp_buf_new(alloc, 4096), imp = rp_buf_new(alloc, 256), a = rp_buf_new(alloc, 64);
    rp_der_put_small(&req, 1);
    rp_der_put_oid(&a, hash_oid(alg), 9);
    rp_der_put_null(&a);
    rp_der_wrap(&imp, RP_DER_SEQUENCE, &a);
    rp_der_put(&imp, RP_DER_OCTET_STRING, digest, rp_hash_size(alg));
    rp_der_wrap(&req, RP_DER_SEQUENCE, &imp);
    rp_der_put_uint(&req, nonce, 8);
    rp_der_put(&req, RP_DER_BOOLEAN, "\xFF", 1);        // certReq: the TSA certificate comes back
    rp_buf_t all = rp_buf_new(alloc, 4096);
    rp_der_wrap(&all, RP_DER_SEQUENCE, &req);
    return rp_buf_take(&all, out, len);
}

// GeneralizedTime YYYYMMDDHHMMSS[.f...]Z.
static bool gen_time(const rp_der_t *e, int64_t *out) {
    const uint8_t *p = e->val.p;
    size_t n = e->val.n;
    if (e->tag != RP_DER_GENTIME || n < 15 || p[n - 1] != 'Z') return false;
    int v[6] = { 0 };
    static const int width[6] = { 4, 2, 2, 2, 2, 2 };
    size_t off = 0;
    for (int f = 0; f < 6; ++f) {
        for (int k = 0; k < width[f]; ++k, ++off) {
            if (p[off] < '0' || p[off] > '9') return false;
            v[f] = v[f] * 10 + (p[off] - '0');
        }
    }
    if (off != n - 1) {                             // a fraction: '.' and digits
        if (p[off] != '.' || off + 2 > n - 1) return false;
        for (size_t k = off + 1; k < n - 1; ++k) {
            if (p[k] < '0' || p[k] > '9') return false;
        }
    }
    if (v[1] < 1 || v[1] > 12 || v[2] < 1 || v[2] > 31 || v[3] > 23 || v[4] > 59 || v[5] > 60) return false;
    int64_t y = v[0] - (v[1] <= 2);
    int64_t era = (y >= 0 ? y : y - 399) / 400, yoe = y - era * 400;
    int64_t doy = (153 * (v[1] + (v[1] > 2 ? -3 : 9)) + 2) / 5 + v[2] - 1;
    int64_t days = era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
    *out = days * 86400 + v[3] * 3600 + v[4] * 60 + v[5];
    return true;
}

#define NO(msg) do { *why = (msg); return false; } while (0)

bool rp_tsp_token(const uint8_t *tok, size_t len, const uint8_t *data, size_t data_len, const uint8_t *nonce, rp_tsp_token_t *t,
                  const char **why) {
    memset(t, 0, sizeof *t);
    rp_der_span_t s = { tok, len };
    rp_der_t ci, oid, w, sd, e, encap, ctype, cw, oct;
    if (!rp_der_get(&s, RP_DER_SEQUENCE, &ci)) NO("the timestamp token is not DER");
    t->token = ci.whole;
    rp_der_span_t cs = rp_der_inside(&ci);
    if (!rp_der_get(&cs, RP_DER_OID, &oid) || !rp_der_oid_is(&oid, O_SIGNED_DATA, 9) || !rp_der_get(&cs, RP_DER_CTX0, &w)) {
        NO("the timestamp token is not CMS SignedData");
    }
    rp_der_span_t ws = rp_der_inside(&w);
    if (!rp_der_get(&ws, RP_DER_SEQUENCE, &sd)) NO("malformed timestamp token");
    rp_der_span_t ss = rp_der_inside(&sd);
    if (!rp_der_get(&ss, RP_DER_INTEGER, &e) || !rp_der_get(&ss, RP_DER_SET, &e) || !rp_der_get(&ss, RP_DER_SEQUENCE, &encap)) {
        NO("malformed timestamp token");
    }
    rp_der_span_t es = rp_der_inside(&encap);
    if (!rp_der_get(&es, RP_DER_OID, &ctype) || !rp_der_oid_is(&ctype, O_TSTINFO, 11) || !rp_der_get(&es, RP_DER_CTX0, &cw)) {
        NO("the token does not hold a TSTInfo");
    }
    rp_der_span_t cws = rp_der_inside(&cw);
    if (!rp_der_get(&cws, RP_DER_OCTET_STRING, &oct)) NO("malformed TSTInfo");
    // TSTInfo.
    rp_der_span_t info_s = { oct.val.p, oct.val.n };
    rp_der_t info, ver, policy, imp, ialg, ihash, serial, gt, x;
    if (!rp_der_get(&info_s, RP_DER_SEQUENCE, &info)) NO("malformed TSTInfo");
    rp_der_span_t is = rp_der_inside(&info);
    uint64_t v;
    if (!rp_der_get(&is, RP_DER_INTEGER, &ver) || !rp_der_small(&ver, &v) || v != 1 || !rp_der_get(&is, RP_DER_OID, &policy) ||
        !rp_der_get(&is, RP_DER_SEQUENCE, &imp) || !rp_der_get(&is, RP_DER_INTEGER, &serial) || !rp_der_get(&is, RP_DER_GENTIME, &gt)) {
        NO("malformed TSTInfo");
    }
    rp_der_span_t ims = rp_der_inside(&imp);
    rp_hash_alg_t ia;
    if (!rp_der_get(&ims, RP_DER_SEQUENCE, &ialg) || !rp_der_get(&ims, RP_DER_OCTET_STRING, &ihash) || !alg_of(&ialg, &ia)) NO("malformed message imprint");
    // The imprint's own hash: a foreign signer may have asked for SHA-384 or SHA-512.
    uint8_t digest[RP_HASH_MAX];
    rp_hash(ia, data, data_len, digest);
    t->imprint_alg = ia;
    if (ihash.val.n != rp_hash_size(ia) || memcmp(ihash.val.p, digest, ihash.val.n) != 0) {
        NO("the timestamp is for other data (its message imprint does not match)");
    }
    if (!gen_time(&gt, &t->gen_time)) NO("a malformed genTime");
    bool nonce_ok = nonce == NULL;
    uint8_t tag;
    if (rp_der_peek(&is, &tag) && tag == RP_DER_SEQUENCE && !rp_der_read(&is, &x)) NO("malformed TSTInfo");    // accuracy
    if (rp_der_peek(&is, &tag) && tag == RP_DER_BOOLEAN && !rp_der_read(&is, &x)) NO("malformed TSTInfo");     // ordering
    if (rp_der_peek(&is, &tag) && tag == RP_DER_INTEGER) {
        rp_der_span_t mag;
        if (!rp_der_read(&is, &x) || !rp_der_uint(&x, &mag)) NO("malformed nonce");
        if (nonce) nonce_ok = mag.n == 8 && memcmp(mag.p, nonce, 8) == 0;
    }
    if (!nonce_ok) NO("the timestamp answers another request (its nonce does not match)");
    // Certificates, then the one signer.
    if (rp_der_peek(&ss, &tag) && tag == RP_DER_CTX0) {
        if (!rp_der_read(&ss, &e)) NO("malformed token certificates");
        t->certs = e.val;
    }
    if (rp_der_peek(&ss, &tag) && tag == RP_DER_CTX1 && !rp_der_read(&ss, &e)) NO("malformed token CRLs");
    rp_der_t sis, si;
    if (!rp_der_get(&ss, RP_DER_SET, &sis)) NO("the token has no signer");
    rp_der_span_t siss = rp_der_inside(&sis);
    if (!rp_der_get(&siss, RP_DER_SEQUENCE, &si) || siss.n) NO("the token does not have exactly one signer");
    rp_der_span_t sif = rp_der_inside(&si);
    rp_der_t sver, sid, halg, attrs, salg, sig;
    if (!rp_der_get(&sif, RP_DER_INTEGER, &sver) || !rp_der_read(&sif, &sid) || !rp_der_get(&sif, RP_DER_SEQUENCE, &halg) ||
        !rp_der_get(&sif, RP_DER_CTX0, &attrs) || !rp_der_get(&sif, RP_DER_SEQUENCE, &salg) || !rp_der_get(&sif, RP_DER_OCTET_STRING, &sig)) {
        NO("malformed token signer");
    }
    rp_hash_alg_t sh;
    if (!alg_of(&halg, &sh)) NO("the token is signed with SHA-1 or an unknown hash");
    rp_der_span_t sas = rp_der_inside(&salg);
    rp_der_t soid;
    if (!rp_der_get(&sas, RP_DER_OID, &soid) || rp_der_oid_is(&soid, O_RSA_PSS, 9)) NO("the token is signed with RSA-PSS, which rubrapack does not verify yet");
    // The signer: IssuerAndSerialNumber, or [0] SubjectKeyIdentifier.
    rp_der_span_t certs = t->certs;
    rp_der_t cert;
    rp_cert_t c = { 0 };
    bool found = false;
    rp_der_t issuer = { 0 }, ser = { 0 };
    if (sid.tag == RP_DER_SEQUENCE) {
        rp_der_span_t ids = rp_der_inside(&sid);
        if (!rp_der_get(&ids, RP_DER_SEQUENCE, &issuer) || !rp_der_get(&ids, RP_DER_INTEGER, &ser)) NO("malformed token signer identifier");
    } else if (sid.tag != 0x80) {
        NO("malformed token signer identifier");
    }
    while (!found && rp_der_get(&certs, RP_DER_SEQUENCE, &cert)) {
        rp_cert_t cc;
        if (!rp_cert_parse(cert.whole.p, cert.whole.n, &cc, NULL)) continue;
        if (sid.tag == RP_DER_SEQUENCE) {
            found = cc.issuer.n == issuer.whole.n && memcmp(cc.issuer.p, issuer.whole.p, cc.issuer.n) == 0 && cc.serial.n == ser.whole.n &&
                    memcmp(cc.serial.p, ser.whole.p, cc.serial.n) == 0;
        } else {
            found = cc.ski.n == sid.val.n && cc.ski.n && memcmp(cc.ski.p, sid.val.p, cc.ski.n) == 0;
        }
        if (found) c = cc;
    }
    if (!found) NO("the token does not carry its signer's certificate (the request asked for it)");
    t->tsa_cert = c.der;
    if (!c.has_eku || !c.eku_time) NO("the token's signer is not a time-stamping certificate");
    if (t->gen_time < c.not_before || t->gen_time > c.not_after) NO("the time-stamping certificate was not valid at the stamped time");
    // Signed attributes.
    uint8_t info_hash[RP_HASH_MAX], attrs_hash[RP_HASH_MAX];
    rp_hash(sh, oct.val.p, oct.val.n, info_hash);
    bool ct_ok = false, md_ok = false;
    rp_der_span_t as = rp_der_inside(&attrs);
    rp_der_t a;
    while (rp_der_get(&as, RP_DER_SEQUENCE, &a)) {
        rp_der_span_t av = rp_der_inside(&a);
        rp_der_t at, set, val;
        if (!rp_der_get(&av, RP_DER_OID, &at) || !rp_der_get(&av, RP_DER_SET, &set)) NO("a malformed token attribute");
        rp_der_span_t vs = rp_der_inside(&set);
        if (!rp_der_read(&vs, &val)) NO("a malformed token attribute");
        if (rp_der_oid_is(&at, O_CONTENT_TYPE, 9)) ct_ok = rp_der_oid_is(&val, O_TSTINFO, 11);
        if (rp_der_oid_is(&at, O_MESSAGE_DIGEST, 9)) {
            md_ok = val.tag == RP_DER_OCTET_STRING && val.val.n == rp_hash_size(sh) && memcmp(val.val.p, info_hash, val.val.n) == 0;
        }
    }
    if (!ct_ok || !md_ok) NO("the token's signed attributes do not match its TSTInfo");
    rp_hash_t h;
    uint8_t set_tag = RP_DER_SET;
    rp_hash_init(&h, sh);
    rp_hash_update(&h, &set_tag, 1);
    rp_hash_update(&h, attrs.whole.p + 1, attrs.whole.n - 1);
    rp_hash_final(&h, attrs_hash);
    if (!rp_cert_verify_sig(&c, sh, attrs_hash, sig.val.p, sig.val.n)) NO("the timestamp token's signature does not verify");
    return true;
}

bool rp_tsp_response(const uint8_t *resp, size_t len, const uint8_t *data, size_t data_len, const uint8_t nonce[8], rp_tsp_token_t *t,
                     const char **why) {
    rp_der_span_t s = { resp, len };
    rp_der_t r, status, st, tok;
    if (!rp_der_get(&s, RP_DER_SEQUENCE, &r)) NO("the timestamp server's answer is not a TimeStampResp");
    rp_der_span_t rs = rp_der_inside(&r);
    uint64_t v;
    if (!rp_der_get(&rs, RP_DER_SEQUENCE, &status)) NO("malformed TimeStampResp");
    rp_der_span_t sts = rp_der_inside(&status);
    if (!rp_der_get(&sts, RP_DER_INTEGER, &st) || !rp_der_small(&st, &v)) NO("malformed TimeStampResp status");
    if (v > 1) NO(v == 2 ? "the timestamp server rejected the request" : v == 3 ? "the timestamp server is waiting (status 3)" : "the timestamp server refused (revocation status)");
    if (!rp_der_get(&rs, RP_DER_SEQUENCE, &tok)) NO("the timestamp server granted but sent no token");
    return rp_tsp_token(tok.whole.p, tok.whole.n, data, data_len, nonce, t, why);
}

proven_err_t rp_tsa_stamp(void *ctx, proven_allocator_t alloc, const uint8_t *sig, size_t sig_len, uint8_t **token, size_t *token_len,
                          const char **why) {
    rp_tsa_t *tsa = ctx;
    tsa->failed = true;                                 // until a checked token is in hand
    uint8_t digest[32], nonce[8];
    rp_hash(RP_HASH_SHA256, sig, sig_len, digest);
    if (!proven_random_bytes(nonce, sizeof nonce)) {
        *why = "the system random source failed";
        return PROVEN_ERR_IO;
    }
    uint8_t *req;
    size_t rl;
    proven_err_t err = rp_tsp_request(alloc, RP_HASH_SHA256, digest, nonce, &req, &rl);
    if (err != PROVEN_OK) return err;
    rp_http_req_t q = { .url = tsa->url, .content_type = "application/timestamp-query", .accept = "application/timestamp-reply", .body = req,
                        .len = rl, .proxy = tsa->proxy, .tls_anchors = tsa->tls_anchors, .tls_anchor_count = tsa->tls_anchor_count };
    rp_http_resp_t r = { 0 };
    err = rp_http_post(alloc, &q, &r, why);
    rp_mem_free(alloc, req);
    if (err != PROVEN_OK) return err;
    rp_tsp_token_t t;
    if (r.status != 200) {
        *why = "the timestamp server answered with an HTTP error";
        err = PROVEN_ERR_IO;
    } else if (!rp_tsp_response(r.body, r.len, sig, sig_len, nonce, &t, why)) {
        err = PROVEN_ERR_INVALID_FORMAT;
    } else if (tsa->anchor_count && !rp_chain_trusted_for(t.tsa_cert, t.certs, tsa->anchors, tsa->anchor_count, t.gen_time,
                                                          RP_PURPOSE_TIMESTAMP, why)) {
        err = PROVEN_ERR_PERMISSION;
    } else {
        uint8_t *copy = rp_mem_alloc(alloc, t.token.n, 1);
        if (copy == NULL) {
            err = PROVEN_ERR_NOMEM;
        } else {
            memcpy(copy, t.token.p, t.token.n);
            *token = copy;
            *token_len = t.token.n;
            tsa->gen_time = t.gen_time;
            tsa->failed = false;
        }
    }
    rp_mem_free(alloc, r.body);
    return err;
}
