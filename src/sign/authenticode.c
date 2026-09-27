// src/sign/authenticode.c - Authenticode SignedData, building and checking (include/rubrapack/sign.h).
//
// CMS (RFC 5652) with the Authenticode rules, each checked against what Windows' signer writes
// (tests/fixtures/authenticode):
//  - the encapsulated content is the SpcIndirectDataContent SEQUENCE itself under [0], not an
//    OCTET STRING;
//  - the messageDigest attribute is the hash of that SEQUENCE's contents only (without its tag and
//    length), while the file digest sits inside it;
//  - the signed attributes are stored as [0] IMPLICIT but signed as a SET (tag 0x31);
//  - a timestamp is one unsigned attribute [1] after the signature value: 1.3.6.1.4.1.311.3.3.1 with
//    the RFC 3161 token, whose imprint is the SHA-256 of the signature value's octets
//    (mssign32!SignerTimeStampEx2, tests/fixtures/timestamp; nothing else in the SignedData changes).

#include "rubrapack/mem.h"
#include "rubrapack/sign.h"

#include <string.h>

#define OID(name, ...) static const uint8_t name[] = { __VA_ARGS__ }
OID(O_SIGNED_DATA, 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x07, 0x02);
OID(O_SHA256, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01);
OID(O_SHA384, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x02);
OID(O_SHA512, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x03);
OID(O_RSA, 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x01);
OID(O_RSA_SHA256, 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x0B);
OID(O_CONTENT_TYPE, 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x09, 0x03);
OID(O_MESSAGE_DIGEST, 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x09, 0x04);
OID(O_SPC_INDIRECT, 0x2B, 0x06, 0x01, 0x04, 0x01, 0x82, 0x37, 0x02, 0x01, 0x04);
OID(O_SPC_OPUS, 0x2B, 0x06, 0x01, 0x04, 0x01, 0x82, 0x37, 0x02, 0x01, 0x0C);
OID(O_SPC_STATEMENT, 0x2B, 0x06, 0x01, 0x04, 0x01, 0x82, 0x37, 0x02, 0x01, 0x0B);
OID(O_SPC_INDIVIDUAL, 0x2B, 0x06, 0x01, 0x04, 0x01, 0x82, 0x37, 0x02, 0x01, 0x15);
OID(O_SPC_RFC3161, 0x2B, 0x06, 0x01, 0x04, 0x01, 0x82, 0x37, 0x03, 0x03, 0x01);
OID(O_COUNTER_SIGNATURE, 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x09, 0x06);

static const uint8_t *hash_oid(rp_hash_alg_t alg, size_t *n) {
    *n = sizeof O_SHA256;
    return alg == RP_HASH_SHA256 ? O_SHA256 : alg == RP_HASH_SHA384 ? O_SHA384 : O_SHA512;
}

static void put_alg(rp_buf_t *b, const uint8_t *oid, size_t n) {
    rp_buf_t a = rp_buf_new(b->alloc, 256);
    rp_der_put_oid(&a, oid, n);
    rp_der_put_null(&a);
    rp_der_wrap(b, RP_DER_SEQUENCE, &a);
}

bool rp_sign_check_key(const rp_keyfile_t *kf, int64_t now, int *leaf, const char **why) {
    *leaf = rp_keyfile_leaf(kf);
    if (*leaf < 0) {
        *why = "no certificate in the key file (or the --cert file) belongs to the key";
        return false;
    }
    rp_cert_t c;
    if (!rp_cert_parse(kf->certs[*leaf], kf->cert_len[*leaf], &c, why)) return false;
    if (!c.has_eku || !(c.eku_code || c.eku_any)) *why = "the certificate is not for code signing (no code-signing extended key usage)";
    else if (c.ca) *why = "the certificate is a CA certificate, not a signing certificate";
    else if (c.has_ku && !c.ku_sign) *why = "the certificate's key usage does not allow digital signatures";
    else if (c.unknown_critical) *why = "the certificate has a critical extension rubrapack does not understand";
    else if (now < c.not_before) *why = "the certificate is not valid yet";
    else if (now > c.not_after) *why = "the certificate has expired";
    else return true;
    return false;
}

// The unsigned attributes [1] with the token `ts` returns for `sig`, after checking it.
static proven_err_t stamp(proven_allocator_t alloc, const rp_timestamper_t *ts, const rp_cert_t *lc, const uint8_t *sig, size_t sig_len,
                          rp_buf_t *ua, const char **why) {
    uint8_t *tok = NULL;
    size_t tl = 0;
    proven_err_t err = ts->stamp(ts->ctx, alloc, sig, sig_len, &tok, &tl, why);
    if (err != PROVEN_OK) return err;
    rp_tsp_token_t t;
    if (!rp_tsp_token(tok, tl, sig, sig_len, NULL, &t, why)) err = PROVEN_ERR_INVALID_FORMAT;
    else if (t.gen_time < lc->not_before || t.gen_time > lc->not_after) {
        *why = "the timestamp server's time lies outside the signing certificate's validity";
        err = PROVEN_ERR_INVALID_STATE;
    }
    if (err == PROVEN_OK) {
        rp_buf_t a = rp_buf_new(alloc, tl + 64), v = rp_buf_new(alloc, tl + 16), at = rp_buf_new(alloc, tl + 64);
        rp_der_put_oid(&a, O_SPC_RFC3161, sizeof O_SPC_RFC3161);
        rp_buf_put(&v, t.token.p, t.token.n);
        rp_der_wrap(&a, RP_DER_SET, &v);
        rp_der_wrap(&at, RP_DER_SEQUENCE, &a);
        rp_der_wrap(ua, RP_DER_CTX1, &at);
        err = ua->err;
    }
    rp_mem_free(alloc, tok);
    return err;
}

proven_err_t rp_authenticode_build(proven_allocator_t alloc, const rp_keyfile_t *kf, int leaf, rp_hash_alg_t alg, const uint8_t *data,
                                   size_t data_len, const uint8_t *digest, const rp_timestamper_t *ts, uint8_t **out, size_t *out_len,
                                   const char **why) {
    size_t hn, hl = rp_hash_size(alg);
    const uint8_t *hoid = hash_oid(alg, &hn);
    rp_cert_t lc;
    if (!rp_cert_parse(kf->certs[leaf], kf->cert_len[leaf], &lc, why)) return PROVEN_ERR_INVALID_ARG;

    // SpcIndirectDataContent { data, DigestInfo { alg, digest } }.
    rp_buf_t ind = rp_buf_new(alloc, 1u << 20), di = rp_buf_new(alloc, 1024);
    rp_buf_put(&ind, data, data_len);
    put_alg(&di, hoid, hn);
    rp_der_put(&di, RP_DER_OCTET_STRING, digest, hl);
    rp_der_wrap(&ind, RP_DER_SEQUENCE, &di);
    uint8_t content_hash[RP_HASH_MAX];
    rp_hash(alg, ind.data, ind.len, content_hash);          // the contents, without the SEQUENCE header
    rp_buf_t indirect = rp_buf_new(alloc, 1u << 20);
    rp_der_wrap(&indirect, RP_DER_SEQUENCE, &ind);

    // Signed attributes, each SEQUENCE { type, SET { value } }.
    rp_buf_t attr[4];
    static const uint8_t opus[] = { 0x30, 0x08, 0xA0, 0x02, 0x80, 0x00, 0xA1, 0x02, 0x80, 0x00 };
    for (int i = 0; i < 4; ++i) attr[i] = rp_buf_new(alloc, 4096);
    rp_buf_t v;
    rp_der_put_oid(&attr[0], O_SPC_OPUS, sizeof O_SPC_OPUS);
    v = rp_buf_new(alloc, 256);
    rp_buf_put(&v, opus, sizeof opus);
    rp_der_wrap(&attr[0], RP_DER_SET, &v);
    rp_der_put_oid(&attr[1], O_CONTENT_TYPE, sizeof O_CONTENT_TYPE);
    v = rp_buf_new(alloc, 256);
    rp_der_put_oid(&v, O_SPC_INDIRECT, sizeof O_SPC_INDIRECT);
    rp_der_wrap(&attr[1], RP_DER_SET, &v);
    rp_der_put_oid(&attr[2], O_SPC_STATEMENT, sizeof O_SPC_STATEMENT);
    v = rp_buf_new(alloc, 256);
    rp_buf_t st = rp_buf_new(alloc, 256);
    rp_der_put_oid(&st, O_SPC_INDIVIDUAL, sizeof O_SPC_INDIVIDUAL);
    rp_der_wrap(&v, RP_DER_SEQUENCE, &st);
    rp_der_wrap(&attr[2], RP_DER_SET, &v);
    rp_der_put_oid(&attr[3], O_MESSAGE_DIGEST, sizeof O_MESSAGE_DIGEST);
    v = rp_buf_new(alloc, 256);
    rp_der_put(&v, RP_DER_OCTET_STRING, content_hash, hl);
    rp_der_wrap(&attr[3], RP_DER_SET, &v);
    rp_buf_t attrs_enc[4];
    rp_der_span_t spans[4];
    for (int i = 0; i < 4; ++i) {
        attrs_enc[i] = rp_buf_new(alloc, 4096);
        rp_der_wrap(&attrs_enc[i], RP_DER_SEQUENCE, &attr[i]);
        spans[i] = (rp_der_span_t){ attrs_enc[i].data, attrs_enc[i].len };
    }
    rp_buf_t signed_set = rp_buf_new(alloc, 16384);
    rp_der_set_of(&signed_set, RP_DER_SET, spans, 4);        // what is signed
    uint8_t attrs_hash[RP_HASH_MAX], sig[512];
    size_t sig_len = 0;
    proven_err_t err = signed_set.err;
    if (err == PROVEN_OK) {
        rp_hash(alg, signed_set.data, signed_set.len, attrs_hash);
        err = rp_rsa_sign(&kf->rsa, alg, attrs_hash, sig, &sig_len);
        if (err != PROVEN_OK) *why = err == PROVEN_ERR_IO ? "the system random source failed" : "the private key does not work (or does not match its certificate)";
    }

    // SignerInfo.
    rp_buf_t si = rp_buf_new(alloc, 1u << 16), ias = rp_buf_new(alloc, 4096);
    rp_der_put_small(&si, 1);
    rp_buf_put(&ias, lc.issuer.p, lc.issuer.n);
    rp_buf_put(&ias, lc.serial.p, lc.serial.n);
    rp_der_wrap(&si, RP_DER_SEQUENCE, &ias);
    put_alg(&si, hoid, hn);
    if (signed_set.len) {
        rp_buf_byte(&si, 0xA0);                               // the same SET, stored as [0] IMPLICIT
        rp_buf_put(&si, signed_set.data + 1, signed_set.len - 1);
    }
    put_alg(&si, O_RSA, sizeof O_RSA);
    rp_der_put(&si, RP_DER_OCTET_STRING, sig, sig_len);
    if (err == PROVEN_OK && ts) {
        rp_buf_t ua = rp_buf_new(alloc, 1u << 16);
        err = stamp(alloc, ts, &lc, sig, sig_len, &ua, why);
        if (err == PROVEN_OK) rp_buf_put(&si, ua.data, ua.len);
        rp_buf_free(&ua);
    }
    rp_buf_t sis = rp_buf_new(alloc, 1u << 16);
    rp_der_wrap(&sis, RP_DER_SEQUENCE, &si);

    // SignedData.
    rp_buf_t sd = rp_buf_new(alloc, 1u << 22), algs = rp_buf_new(alloc, 256), encap = rp_buf_new(alloc, 1u << 20),
             certs = rp_buf_new(alloc, 1u << 22);
    rp_der_put_small(&sd, 1);
    put_alg(&algs, hoid, hn);
    rp_der_wrap(&sd, RP_DER_SET, &algs);
    rp_der_put_oid(&encap, O_SPC_INDIRECT, sizeof O_SPC_INDIRECT);
    rp_der_wrap(&encap, RP_DER_CTX0, &indirect);
    rp_der_wrap(&sd, RP_DER_SEQUENCE, &encap);
    // The leaf first, then the other certificates, without self-signed roots (as Windows does).
    rp_buf_put(&certs, kf->certs[leaf], kf->cert_len[leaf]);
    for (size_t i = 0; i < kf->cert_count; ++i) {
        rp_cert_t c;
        if ((int)i == leaf || !rp_cert_parse(kf->certs[i], kf->cert_len[i], &c, NULL)) continue;
        if (c.issuer.n == c.subject.n && memcmp(c.issuer.p, c.subject.p, c.issuer.n) == 0) continue;
        rp_buf_put(&certs, kf->certs[i], kf->cert_len[i]);
    }
    rp_der_wrap(&sd, RP_DER_CTX0, &certs);
    rp_der_wrap(&sd, RP_DER_SET, &sis);
    rp_buf_t ci = rp_buf_new(alloc, 1u << 22), sdw = rp_buf_new(alloc, 1u << 22);
    rp_der_wrap(&sdw, RP_DER_SEQUENCE, &sd);
    rp_der_put_oid(&ci, O_SIGNED_DATA, sizeof O_SIGNED_DATA);
    rp_der_wrap(&ci, RP_DER_CTX0, &sdw);
    rp_buf_t all = rp_buf_new(alloc, 1u << 22);
    rp_der_wrap(&all, RP_DER_SEQUENCE, &ci);
    for (int i = 0; i < 4; ++i) rp_buf_free(&attrs_enc[i]);
    rp_buf_free(&signed_set);
    if (err != PROVEN_OK) {
        rp_buf_free(&all);
        return err;
    }
    if (all.err != PROVEN_OK) {
        *why = "out of memory";
        err = all.err;
        rp_buf_free(&all);
        return err;
    }
    return rp_buf_take(&all, out, out_len);
}

// ---- checking -----------------------------------------------------------------------------------

static bool alg_of(const rp_der_t *algid, rp_hash_alg_t *alg) {
    rp_der_span_t s = rp_der_inside(algid);
    rp_der_t oid;
    if (!rp_der_get(&s, RP_DER_OID, &oid)) return false;
    if (rp_der_oid_is(&oid, O_SHA256, sizeof O_SHA256)) *alg = RP_HASH_SHA256;
    else if (rp_der_oid_is(&oid, O_SHA384, sizeof O_SHA384)) *alg = RP_HASH_SHA384;
    else if (rp_der_oid_is(&oid, O_SHA512, sizeof O_SHA512)) *alg = RP_HASH_SHA512;
    else return false;
    return true;
}

#define STOP(msg) do { *why = (msg); return; } while (0)

void rp_authenticode_verify(const uint8_t *der, size_t len, const uint8_t *digest, rp_authenticode_check_t *r, const char **why) {
    memset(r, 0, sizeof *r);
    const char *dummy;
    if (why == NULL) why = &dummy;
    *why = NULL;
    rp_der_span_t s = { der, len };
    rp_der_t ci, oid, w, sd, ver, algs, encap, ctype, cw, content, e;
    if (!rp_der_get(&s, RP_DER_SEQUENCE, &ci)) STOP("the signature is not a CMS ContentInfo");
    rp_der_span_t cs = rp_der_inside(&ci);
    if (!rp_der_get(&cs, RP_DER_OID, &oid) || !rp_der_oid_is(&oid, O_SIGNED_DATA, sizeof O_SIGNED_DATA) || !rp_der_get(&cs, RP_DER_CTX0, &w)) {
        STOP("the signature is not CMS SignedData");
    }
    rp_der_span_t ws = rp_der_inside(&w);
    if (!rp_der_get(&ws, RP_DER_SEQUENCE, &sd)) STOP("malformed SignedData");
    rp_der_span_t ss = rp_der_inside(&sd);
    if (!rp_der_get(&ss, RP_DER_INTEGER, &ver) || !rp_der_get(&ss, RP_DER_SET, &algs) || !rp_der_get(&ss, RP_DER_SEQUENCE, &encap)) {
        STOP("malformed SignedData");
    }
    rp_der_span_t es = rp_der_inside(&encap);
    if (!rp_der_get(&es, RP_DER_OID, &ctype) || !rp_der_oid_is(&ctype, O_SPC_INDIRECT, sizeof O_SPC_INDIRECT) || !rp_der_get(&es, RP_DER_CTX0, &cw)) {
        STOP("the signed content is not Authenticode indirect data");
    }
    rp_der_span_t cws = rp_der_inside(&cw);
    if (!rp_der_get(&cws, RP_DER_SEQUENCE, &content)) STOP("malformed indirect data");
    rp_der_span_t ins = rp_der_inside(&content);
    rp_der_t data, dinfo, dalg, dval;
    if (!rp_der_get(&ins, RP_DER_SEQUENCE, &data) || !rp_der_get(&ins, RP_DER_SEQUENCE, &dinfo)) STOP("malformed indirect data");
    rp_der_span_t ds = rp_der_inside(&dinfo);
    if (!rp_der_get(&ds, RP_DER_SEQUENCE, &dalg) || !rp_der_get(&ds, RP_DER_OCTET_STRING, &dval) || !alg_of(&dalg, &r->alg) ||
        dval.val.n != rp_hash_size(r->alg)) {
        STOP("the signed digest uses an unknown hash");
    }
    r->data = data.whole;
    if (digest) r->digest_ok = memcmp(digest, dval.val.p, dval.val.n) == 0;
    uint8_t tag;
    if (rp_der_peek(&ss, &tag) && tag == RP_DER_CTX0) {
        if (!rp_der_read(&ss, &e)) STOP("malformed certificates");
        r->certs = e.val;
    }
    if (rp_der_peek(&ss, &tag) && tag == RP_DER_CTX1 && !rp_der_read(&ss, &e)) STOP("malformed revocation data");
    rp_der_t sis, si;
    if (!rp_der_get(&ss, RP_DER_SET, &sis)) STOP("no signer");
    rp_der_span_t siss = rp_der_inside(&sis);
    if (!rp_der_get(&siss, RP_DER_SEQUENCE, &si) || siss.n) STOP("not exactly one signer");
    rp_der_span_t sif = rp_der_inside(&si);
    rp_der_t sver, ias, halg, attrs, salg, sig, issuer, serial;
    if (!rp_der_get(&sif, RP_DER_INTEGER, &sver) || !rp_der_get(&sif, RP_DER_SEQUENCE, &ias) || !rp_der_get(&sif, RP_DER_SEQUENCE, &halg) ||
        !rp_der_get(&sif, RP_DER_CTX0, &attrs) || !rp_der_get(&sif, RP_DER_SEQUENCE, &salg) || !rp_der_get(&sif, RP_DER_OCTET_STRING, &sig)) {
        STOP("malformed signer information");
    }
    rp_hash_alg_t sh;
    if (!alg_of(&halg, &sh) || sh != r->alg) STOP("the signer uses another hash than the content");
    rp_der_span_t iass = rp_der_inside(&ias);
    if (!rp_der_get(&iass, RP_DER_SEQUENCE, &issuer) || !rp_der_get(&iass, RP_DER_INTEGER, &serial)) STOP("malformed signer identifier");
    r->parsed = true;
    // Attributes: contentType and messageDigest must match the content.
    uint8_t content_hash[RP_HASH_MAX];
    rp_hash(r->alg, content.val.p, content.val.n, content_hash);
    bool ct_ok = false, md_ok = false;
    rp_der_span_t as = rp_der_inside(&attrs);
    rp_der_t a;
    while (rp_der_get(&as, RP_DER_SEQUENCE, &a)) {
        rp_der_span_t av = rp_der_inside(&a);
        rp_der_t at, set, val;
        if (!rp_der_get(&av, RP_DER_OID, &at) || !rp_der_get(&av, RP_DER_SET, &set)) STOP("a malformed signed attribute");
        rp_der_span_t vs = rp_der_inside(&set);
        if (!rp_der_read(&vs, &val)) STOP("a malformed signed attribute");
        if (rp_der_oid_is(&at, O_CONTENT_TYPE, sizeof O_CONTENT_TYPE)) ct_ok = rp_der_oid_is(&val, O_SPC_INDIRECT, sizeof O_SPC_INDIRECT);
        if (rp_der_oid_is(&at, O_MESSAGE_DIGEST, sizeof O_MESSAGE_DIGEST)) {
            md_ok = val.tag == RP_DER_OCTET_STRING && val.val.n == rp_hash_size(r->alg) && memcmp(val.val.p, content_hash, val.val.n) == 0;
        }
    }
    r->attrs_ok = ct_ok && md_ok;
    // The signer's certificate, by issuer and serial number.
    rp_der_span_t certs = r->certs;
    rp_der_t cert;
    rp_cert_t c = { 0 };
    bool found = false;
    while (!found && rp_der_get(&certs, RP_DER_SEQUENCE, &cert)) {
        rp_cert_t cc;
        if (rp_cert_parse(cert.whole.p, cert.whole.n, &cc, NULL) && cc.issuer.n == issuer.whole.n &&
            memcmp(cc.issuer.p, issuer.whole.p, cc.issuer.n) == 0 && cc.serial.n == serial.whole.n &&
            memcmp(cc.serial.p, serial.whole.p, cc.serial.n) == 0) {
            c = cc;
            found = true;
        }
    }
    if (!found || !c.rsa) STOP("the signer's certificate is not in the signature");
    r->signer_cert = c.der;
    rp_der_span_t sa = rp_der_inside(&salg);
    rp_der_t soid;
    if (!rp_der_get(&sa, RP_DER_OID, &soid) || !(rp_der_oid_is(&soid, O_RSA, sizeof O_RSA) || rp_der_oid_is(&soid, O_RSA_SHA256, sizeof O_RSA_SHA256))) {
        STOP("the signature is not RSA");
    }
    // Signed as a SET: the stored [0] with its tag changed.
    rp_hash_t h;
    uint8_t set_tag = RP_DER_SET, attrs_hash[RP_HASH_MAX];
    rp_hash_init(&h, r->alg);
    rp_hash_update(&h, &set_tag, 1);
    rp_hash_update(&h, attrs.whole.p + 1, attrs.whole.n - 1);
    rp_hash_final(&h, attrs_hash);
    r->signature_ok = rp_rsa_verify(c.rsa_n.p, c.rsa_n.n, c.rsa_e.p, c.rsa_e.n, r->alg, attrs_hash, sig.val.p, sig.val.n);
    r->sig = sig.val;
    // Unsigned attributes: at most one timestamp, RFC 3161 or the old counterSignature.
    rp_der_t ua;
    if (rp_der_peek(&sif, &tag) && tag == RP_DER_CTX1) {
        rp_der_span_t us = { NULL, 0 };
        if (!rp_der_read(&sif, &ua)) r->ts_bad = true;
        else us = rp_der_inside(&ua);
        int stamps = 0;
        while (!r->ts_bad && us.n) {
            rp_der_t at, set, val;
            if (!rp_der_get(&us, RP_DER_SEQUENCE, &a)) {
                r->ts_bad = true;
                break;
            }
            rp_der_span_t av = rp_der_inside(&a);
            if (!rp_der_get(&av, RP_DER_OID, &at) || !rp_der_get(&av, RP_DER_SET, &set)) {
                r->ts_bad = true;
                break;
            }
            rp_der_span_t vs = rp_der_inside(&set);
            bool rfc = rp_der_oid_is(&at, O_SPC_RFC3161, sizeof O_SPC_RFC3161), old = rp_der_oid_is(&at, O_COUNTER_SIGNATURE, sizeof O_COUNTER_SIGNATURE);
            if (!rfc && !old) continue;                         // another unsigned attribute (a nested signature, say)
            if (++stamps > 1 || !rp_der_get(&vs, RP_DER_SEQUENCE, &val) || vs.n) {
                r->ts_bad = true;
                break;
            }
            if (rfc) r->ts_token = val.whole;
            else r->ts_legacy = true;
        }
    }
    if (!r->attrs_ok) *why = "the signed attributes do not match the signed content";
    else if (!r->signature_ok) *why = "the signature does not verify with the signer's certificate";
}
