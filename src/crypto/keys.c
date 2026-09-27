// src/crypto/keys.c - key files: PEM (RFC 7468), PKCS#8 (RFC 5958) with PBES2 (RFC 8018), PKCS#12
// (RFC 7292) - include/rubrapack/pki.h. New code (lowent_lang has none of these).
//
// PKCS#12 has two password uses that must not be mixed up (RFC-0001 12.3.1): the MacData key comes
// from the RFC 7292 appendix B KDF over the password as a BMPString (UTF-16BE with a closing NUL),
// while PBES2 encryption feeds the password's UTF-8 bytes to PBKDF2. The MAC is checked before any
// decryption, in constant time.

#include "rubrapack/limits.h"
#include "rubrapack/mem.h"
#include "rubrapack/pki.h"
#include "rubrapack/text.h"

#include <stdio.h>
#include <string.h>

enum { MAX_ITER = 1000000, MAX_FILE = 1u << 22 };

#define OID(name, ...) static const uint8_t name[] = { __VA_ARGS__ }
OID(O_DATA, 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x07, 0x01);
OID(O_ENCDATA, 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x07, 0x06);
OID(O_KEYBAG, 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x0C, 0x0A, 0x01, 0x01);
OID(O_SHROUDED, 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x0C, 0x0A, 0x01, 0x02);
OID(O_CERTBAG, 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x0C, 0x0A, 0x01, 0x03);
OID(O_X509CERT, 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x09, 0x16, 0x01);
OID(O_PBES2, 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x05, 0x0D);
OID(O_PBKDF2, 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x05, 0x0C);
OID(O_HMAC256, 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x02, 0x09);
OID(O_HMAC384, 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x02, 0x0A);
OID(O_HMAC512, 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x02, 0x0B);
OID(O_AES128CBC, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x01, 0x02);
OID(O_AES192CBC, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x01, 0x16);
OID(O_AES256CBC, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x01, 0x2A);
OID(O_SHA256, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01);
OID(O_SHA384, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x02);
OID(O_SHA512, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x03);
OID(O_RSA, 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x01);
OID(O_ECKEY, 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x02, 0x01);

typedef struct {
    proven_allocator_t alloc;
    const uint8_t     *pass;
    size_t             pass_len;
    rp_keyfile_t      *out;
    const char       **why;
    proven_err_t       err;
} ctx_t;

static bool fail(ctx_t *c, proven_err_t err, const char *why) {
    if (c->err == PROVEN_OK) {
        c->err = err;
        *c->why = why;
    }
    return false;
}

// ---- certificates and keys into the result -------------------------------------------------------

static bool add_cert(ctx_t *c, const uint8_t *der, size_t len) {
    rp_cert_t cert;
    const char *why;
    if (!rp_cert_parse(der, len, &cert, &why)) return fail(c, PROVEN_ERR_INVALID_FORMAT, "a certificate in the file is malformed");
    rp_keyfile_t *kf = c->out;
    uint8_t **nc = rp_mem_alloc(c->alloc, kf->cert_count + 1, sizeof *nc);
    size_t *nl = rp_mem_alloc(c->alloc, kf->cert_count + 1, sizeof *nl);
    uint8_t *copy = rp_mem_alloc(c->alloc, len, 1);
    if (nc == NULL || nl == NULL || copy == NULL) {
        rp_mem_free(c->alloc, nc);
        rp_mem_free(c->alloc, nl);
        rp_mem_free(c->alloc, copy);
        return fail(c, PROVEN_ERR_NOMEM, "out of memory");
    }
    if (kf->cert_count) {
        memcpy(nc, kf->certs, kf->cert_count * sizeof *nc);
        memcpy(nl, kf->cert_len, kf->cert_count * sizeof *nl);
    }
    rp_mem_free(c->alloc, kf->certs);
    rp_mem_free(c->alloc, kf->cert_len);
    memcpy(copy, der, len);
    nc[kf->cert_count] = copy;
    nl[kf->cert_count] = len;
    kf->certs = nc;
    kf->cert_len = nl;
    kf->cert_count++;
    return true;
}

// PrivateKeyInfo (RFC 5958): an RSA key is kept; an EC key is refused for now (RFC-0007 S1).
static bool take_pkcs8(ctx_t *c, const uint8_t *der, size_t len) {
    if (c->out->key_der) return fail(c, PROVEN_ERR_INVALID_FORMAT, "the file holds more than one private key");
    rp_der_span_t s = { der, len };
    rp_der_t info, ver, alg, oid, key;
    if (!rp_der_get(&s, RP_DER_SEQUENCE, &info)) return fail(c, PROVEN_ERR_INVALID_FORMAT, "a malformed private key");
    rp_der_span_t is = rp_der_inside(&info);
    uint64_t v;
    uint8_t second = 0;
    rp_der_span_t look = is;
    rp_der_t skip;
    if (rp_der_read(&look, &skip) && rp_der_peek(&look, &second) && second == RP_DER_INTEGER) {
        return fail(c, PROVEN_ERR_UNSUPPORTED, "an old-style RSA private key (PKCS#1); convert it to PKCS#8 (BEGIN PRIVATE KEY or ENCRYPTED PRIVATE KEY)");
    }
    if (!rp_der_get(&is, RP_DER_INTEGER, &ver) || !rp_der_small(&ver, &v) || v > 1 || !rp_der_get(&is, RP_DER_SEQUENCE, &alg) ||
        !rp_der_get(&is, RP_DER_OCTET_STRING, &key)) {
        return fail(c, PROVEN_ERR_INVALID_FORMAT, "a malformed private key");
    }
    rp_der_span_t as = rp_der_inside(&alg);
    if (!rp_der_get(&as, RP_DER_OID, &oid)) return fail(c, PROVEN_ERR_INVALID_FORMAT, "a private key without an algorithm");
    if (rp_der_oid_is(&oid, O_ECKEY, sizeof O_ECKEY)) {
        return fail(c, PROVEN_ERR_UNSUPPORTED, "EC keys are not enabled yet (only RSA keys sign until Windows accepts ECDSA in each format, RFC-0007 S1)");
    }
    if (!rp_der_oid_is(&oid, O_RSA, sizeof O_RSA)) return fail(c, PROVEN_ERR_UNSUPPORTED, "the private key is not an RSA key");
    uint8_t *copy = rp_mem_alloc(c->alloc, key.val.n, 1);
    if (copy == NULL) return fail(c, PROVEN_ERR_NOMEM, "out of memory");
    memcpy(copy, key.val.p, key.val.n);
    rp_der_span_t ks = { copy, key.val.n };
    rp_der_t seq, f[9];
    rp_der_span_t mag[9];
    bool ok = rp_der_get(&ks, RP_DER_SEQUENCE, &seq);
    rp_der_span_t fs = ok ? rp_der_inside(&seq) : (rp_der_span_t){ 0 };
    for (int i = 0; ok && i < 9; ++i) ok = rp_der_get(&fs, RP_DER_INTEGER, &f[i]) && rp_der_uint(&f[i], &mag[i]);
    uint64_t kv = 1;
    if (ok) ok = rp_der_small(&f[0], &kv) && kv == 0;       // two-prime keys only
    if (!ok) {
        rp_wipe(copy, key.val.n);
        rp_mem_free(c->alloc, copy);
        return fail(c, PROVEN_ERR_UNSUPPORTED, "not a two-prime RSA private key");
    }
    rp_keyfile_t *kf = c->out;
    kf->key_der = copy;
    kf->key_len = key.val.n;
    kf->rsa = (rp_rsa_key_t){ mag[1].p, mag[2].p, mag[3].p, mag[4].p, mag[5].p, mag[6].p, mag[7].p, mag[8].p,
                              mag[1].n, mag[2].n, mag[3].n, mag[4].n, mag[5].n, mag[6].n, mag[7].n, mag[8].n };
    return true;
}

// ---- PBES2 ------------------------------------------------------------------------------------

// Decrypts `data` in place under an AlgorithmIdentifier that must be PBES2; *plain_len gets the
// length without the padding. The password is used as bytes (UTF-8), as RFC 8018 does.
static bool pbes2_decrypt(ctx_t *c, const rp_der_t *alg, uint8_t *data, size_t len, size_t *plain_len) {
    rp_der_span_t as = rp_der_inside(alg);
    rp_der_t oid, params, kdf, kdf_oid, kdf_params, salt, iter, enc, enc_oid, iv, e;
    if (!rp_der_get(&as, RP_DER_OID, &oid)) return fail(c, PROVEN_ERR_INVALID_FORMAT, "a malformed encryption algorithm");
    if (!rp_der_oid_is(&oid, O_PBES2, sizeof O_PBES2)) {
        return fail(c, PROVEN_ERR_UNSUPPORTED, "the file is encrypted with an old algorithm (3DES or RC2); export it again with AES-256 (PBES2)");
    }
    if (!rp_der_get(&as, RP_DER_SEQUENCE, &params)) return fail(c, PROVEN_ERR_INVALID_FORMAT, "malformed PBES2 parameters");
    rp_der_span_t ps = rp_der_inside(&params);
    if (!rp_der_get(&ps, RP_DER_SEQUENCE, &kdf) || !rp_der_get(&ps, RP_DER_SEQUENCE, &enc)) {
        return fail(c, PROVEN_ERR_INVALID_FORMAT, "malformed PBES2 parameters");
    }
    rp_der_span_t ks = rp_der_inside(&kdf);
    if (!rp_der_get(&ks, RP_DER_OID, &kdf_oid) || !rp_der_oid_is(&kdf_oid, O_PBKDF2, sizeof O_PBKDF2) ||
        !rp_der_get(&ks, RP_DER_SEQUENCE, &kdf_params)) {
        return fail(c, PROVEN_ERR_UNSUPPORTED, "PBES2 with a key derivation other than PBKDF2");
    }
    rp_der_span_t kps = rp_der_inside(&kdf_params);
    uint64_t iterations = 0, key_len = 0;
    if (!rp_der_get(&kps, RP_DER_OCTET_STRING, &salt) || !rp_der_get(&kps, RP_DER_INTEGER, &iter) || !rp_der_small(&iter, &iterations)) {
        return fail(c, PROVEN_ERR_INVALID_FORMAT, "malformed PBKDF2 parameters");
    }
    if (rp_der_get(&kps, RP_DER_INTEGER, &e) && !rp_der_small(&e, &key_len)) return fail(c, PROVEN_ERR_INVALID_FORMAT, "malformed PBKDF2 parameters");
    rp_hash_alg_t prf = (rp_hash_alg_t)-1;
    if (rp_der_get(&kps, RP_DER_SEQUENCE, &e)) {
        rp_der_span_t prs = rp_der_inside(&e);
        rp_der_t po;
        if (rp_der_get(&prs, RP_DER_OID, &po)) {
            if (rp_der_oid_is(&po, O_HMAC256, sizeof O_HMAC256)) prf = RP_HASH_SHA256;
            else if (rp_der_oid_is(&po, O_HMAC384, sizeof O_HMAC384)) prf = RP_HASH_SHA384;
            else if (rp_der_oid_is(&po, O_HMAC512, sizeof O_HMAC512)) prf = RP_HASH_SHA512;
        }
    }
    if ((int)prf < 0) return fail(c, PROVEN_ERR_UNSUPPORTED, "PBKDF2 with HMAC-SHA1 (the default) or an unknown PRF; use HMAC-SHA256");
    if (iterations < 1 || iterations > MAX_ITER) return fail(c, PROVEN_ERR_UNSUPPORTED, "a PBKDF2 iteration count of 0 or above 1,000,000");
    rp_der_span_t es = rp_der_inside(&enc);
    size_t aes_len = 0;
    if (!rp_der_get(&es, RP_DER_OID, &enc_oid)) return fail(c, PROVEN_ERR_INVALID_FORMAT, "malformed PBES2 parameters");
    if (rp_der_oid_is(&enc_oid, O_AES256CBC, sizeof O_AES256CBC)) aes_len = 32;
    else if (rp_der_oid_is(&enc_oid, O_AES192CBC, sizeof O_AES192CBC)) aes_len = 24;
    else if (rp_der_oid_is(&enc_oid, O_AES128CBC, sizeof O_AES128CBC)) aes_len = 16;
    else return fail(c, PROVEN_ERR_UNSUPPORTED, "PBES2 with a cipher other than AES-CBC");
    if (key_len && key_len != aes_len) return fail(c, PROVEN_ERR_INVALID_FORMAT, "PBKDF2 key length does not match the cipher");
    if (!rp_der_get(&es, RP_DER_OCTET_STRING, &iv) || iv.val.n != 16) return fail(c, PROVEN_ERR_INVALID_FORMAT, "a malformed AES IV");
    if (len == 0 || len % 16) return fail(c, PROVEN_ERR_INVALID_FORMAT, "encrypted data is not a whole number of AES blocks");
    uint8_t key[32];
    rp_pbkdf2(prf, c->pass, c->pass_len, salt.val.p, salt.val.n, (uint32_t)iterations, key, aes_len);
    rp_aes_t aes;
    (void)rp_aes_init(&aes, key, aes_len);
    rp_wipe(key, sizeof key);
    (void)rp_aes_cbc_decrypt(&aes, iv.val.p, data, data, len);
    rp_wipe(&aes, sizeof aes);
    uint8_t pad = data[len - 1];
    bool ok = pad >= 1 && pad <= 16;
    for (size_t i = 0; ok && i < pad; ++i) ok = data[len - 1 - i] == pad;
    if (!ok) return fail(c, PROVEN_ERR_PERMISSION, "wrong password (the decrypted data does not end in valid padding)");
    *plain_len = len - pad;
    return true;
}

static bool take_encrypted_pkcs8(ctx_t *c, const uint8_t *der, size_t len) {
    rp_der_span_t s = { der, len };
    rp_der_t info, alg, data;
    if (!rp_der_get(&s, RP_DER_SEQUENCE, &info)) return fail(c, PROVEN_ERR_INVALID_FORMAT, "a malformed encrypted private key");
    rp_der_span_t is = rp_der_inside(&info);
    if (!rp_der_get(&is, RP_DER_SEQUENCE, &alg) || !rp_der_get(&is, RP_DER_OCTET_STRING, &data)) {
        return fail(c, PROVEN_ERR_INVALID_FORMAT, "a malformed encrypted private key");
    }
    if (c->pass == NULL) return fail(c, PROVEN_ERR_PERMISSION, "the key is encrypted: give its password (--pass-env or --pass-file)");
    uint8_t *buf = rp_mem_alloc(c->alloc, data.val.n + 1, 1);
    if (buf == NULL) return fail(c, PROVEN_ERR_NOMEM, "out of memory");
    memcpy(buf, data.val.p, data.val.n);
    size_t plain = 0;
    bool ok = pbes2_decrypt(c, &alg, buf, data.val.n, &plain) && take_pkcs8(c, buf, plain);
    rp_wipe(buf, data.val.n);
    rp_mem_free(c->alloc, buf);
    return ok;
}

// ---- PKCS#12 -----------------------------------------------------------------------------------

// RFC 7292 appendix B.2 with id 3 (MAC key); `bmp` is the password as a BMPString with its NUL.
static void p12_kdf(rp_hash_alg_t alg, const uint8_t *bmp, size_t bmp_len, const uint8_t *salt, size_t salt_len,
                    uint32_t iterations, uint8_t *out, size_t out_len) {
    size_t u = rp_hash_size(alg), v = rp_hash_block(alg);
    uint8_t d[128], i_buf[2 * 1024 + 256], a[RP_HASH_MAX], b[128];
    memset(d, 3, v);
    size_t s_len = salt_len ? v * ((salt_len + v - 1) / v) : 0, p_len = bmp_len ? v * ((bmp_len + v - 1) / v) : 0;
    size_t il = s_len + p_len;
    for (size_t k = 0; k < s_len; ++k) i_buf[k] = salt[k % salt_len];
    for (size_t k = 0; k < p_len; ++k) i_buf[s_len + k] = bmp[k % bmp_len];
    for (size_t done = 0; done < out_len;) {
        rp_hash_t h;
        rp_hash_init(&h, alg);
        rp_hash_update(&h, d, v);
        rp_hash_update(&h, i_buf, il);
        rp_hash_final(&h, a);
        for (uint32_t r = 1; r < iterations; ++r) rp_hash(alg, a, u, a);
        size_t take = out_len - done < u ? out_len - done : u;
        memcpy(out + done, a, take);
        done += take;
        if (done >= out_len) break;
        for (size_t k = 0; k < v; ++k) b[k] = a[k % u];
        for (size_t j = 0; j < il; j += v) {            // I_j = (I_j + B + 1) mod 2^(8v)
            unsigned carry = 1;
            for (size_t k = v; k-- > 0;) {
                unsigned sum = i_buf[j + k] + b[k] + carry;
                i_buf[j + k] = (uint8_t)sum;
                carry = sum >> 8;
            }
        }
    }
    rp_wipe(i_buf, sizeof i_buf);
    rp_wipe(a, sizeof a);
    rp_wipe(b, sizeof b);
}

static bool safe_contents(ctx_t *c, const uint8_t *der, size_t len);

// One ContentInfo of the AuthenticatedSafe: data (plain SafeContents) or encryptedData.
static bool auth_content(ctx_t *c, const rp_der_t *ci) {
    rp_der_span_t s = rp_der_inside(ci);
    rp_der_t type, wrap, e;
    if (!rp_der_get(&s, RP_DER_OID, &type) || !rp_der_get(&s, RP_DER_CTX0, &wrap)) return fail(c, PROVEN_ERR_INVALID_FORMAT, "a malformed PKCS#12 content");
    rp_der_span_t ws = rp_der_inside(&wrap);
    if (rp_der_oid_is(&type, O_DATA, sizeof O_DATA)) {
        if (!rp_der_get(&ws, RP_DER_OCTET_STRING, &e)) return fail(c, PROVEN_ERR_INVALID_FORMAT, "a malformed PKCS#12 content");
        return safe_contents(c, e.val.p, e.val.n);
    }
    if (!rp_der_oid_is(&type, O_ENCDATA, sizeof O_ENCDATA)) return fail(c, PROVEN_ERR_UNSUPPORTED, "a PKCS#12 content that is neither data nor password-encrypted data");
    rp_der_t ed, ver, eci, ctype, alg, content;
    if (!rp_der_get(&ws, RP_DER_SEQUENCE, &ed)) return fail(c, PROVEN_ERR_INVALID_FORMAT, "malformed encrypted PKCS#12 data");
    rp_der_span_t es = rp_der_inside(&ed);
    if (!rp_der_get(&es, RP_DER_INTEGER, &ver) || !rp_der_get(&es, RP_DER_SEQUENCE, &eci)) return fail(c, PROVEN_ERR_INVALID_FORMAT, "malformed encrypted PKCS#12 data");
    rp_der_span_t cs = rp_der_inside(&eci);
    if (!rp_der_get(&cs, RP_DER_OID, &ctype) || !rp_der_get(&cs, RP_DER_SEQUENCE, &alg) || !rp_der_get(&cs, 0x80, &content)) {
        return fail(c, PROVEN_ERR_INVALID_FORMAT, "malformed encrypted PKCS#12 data");
    }
    if (c->pass == NULL) return fail(c, PROVEN_ERR_PERMISSION, "the file is encrypted: give its password (--pass-env or --pass-file)");
    uint8_t *buf = rp_mem_alloc(c->alloc, content.val.n + 1, 1);
    if (buf == NULL) return fail(c, PROVEN_ERR_NOMEM, "out of memory");
    memcpy(buf, content.val.p, content.val.n);
    size_t plain = 0;
    bool ok = pbes2_decrypt(c, &alg, buf, content.val.n, &plain) && safe_contents(c, buf, plain);
    rp_wipe(buf, content.val.n);
    rp_mem_free(c->alloc, buf);
    return ok;
}

static bool safe_contents(ctx_t *c, const uint8_t *der, size_t len) {
    rp_der_span_t s = { der, len };
    rp_der_t seq, bag, id, val;
    if (!rp_der_get(&s, RP_DER_SEQUENCE, &seq)) return fail(c, PROVEN_ERR_INVALID_FORMAT, "a malformed PKCS#12 bag list");
    rp_der_span_t bs = rp_der_inside(&seq);
    while (rp_der_get(&bs, RP_DER_SEQUENCE, &bag)) {
        rp_der_span_t b = rp_der_inside(&bag);
        if (!rp_der_get(&b, RP_DER_OID, &id) || !rp_der_get(&b, RP_DER_CTX0, &val)) return fail(c, PROVEN_ERR_INVALID_FORMAT, "a malformed PKCS#12 bag");
        rp_der_span_t vs = rp_der_inside(&val);
        rp_der_t inner;
        if (!rp_der_read(&vs, &inner)) return fail(c, PROVEN_ERR_INVALID_FORMAT, "a malformed PKCS#12 bag");
        if (rp_der_oid_is(&id, O_KEYBAG, sizeof O_KEYBAG)) {
            if (!take_pkcs8(c, inner.whole.p, inner.whole.n)) return false;
        } else if (rp_der_oid_is(&id, O_SHROUDED, sizeof O_SHROUDED)) {
            if (!take_encrypted_pkcs8(c, inner.whole.p, inner.whole.n)) return false;
        } else if (rp_der_oid_is(&id, O_CERTBAG, sizeof O_CERTBAG)) {
            rp_der_span_t cb = rp_der_inside(&inner);
            rp_der_t ct, cv, oct;
            if (!rp_der_get(&cb, RP_DER_OID, &ct) || !rp_der_get(&cb, RP_DER_CTX0, &cv)) return fail(c, PROVEN_ERR_INVALID_FORMAT, "a malformed certificate bag");
            if (!rp_der_oid_is(&ct, O_X509CERT, sizeof O_X509CERT)) continue;       // other certificate kinds are skipped
            rp_der_span_t cvs = rp_der_inside(&cv);
            if (!rp_der_get(&cvs, RP_DER_OCTET_STRING, &oct)) return fail(c, PROVEN_ERR_INVALID_FORMAT, "a malformed certificate bag");
            if (!add_cert(c, oct.val.p, oct.val.n)) return false;
        }
        // CRL and secret bags are not needed and are skipped.
    }
    if (bs.n) return fail(c, PROVEN_ERR_INVALID_FORMAT, "a malformed PKCS#12 bag list");
    return true;
}

static bool load_pfx(ctx_t *c, const uint8_t *der, size_t len) {
    rp_der_span_t s = { der, len };
    rp_der_t pfx, ver, auth, type, wrap, oct, mac;
    uint64_t v;
    if (!rp_der_get(&s, RP_DER_SEQUENCE, &pfx)) return fail(c, PROVEN_ERR_INVALID_FORMAT, "not a PKCS#12 file");
    rp_der_span_t ps = rp_der_inside(&pfx);
    if (!rp_der_get(&ps, RP_DER_INTEGER, &ver) || !rp_der_small(&ver, &v) || v != 3 || !rp_der_get(&ps, RP_DER_SEQUENCE, &auth)) {
        return fail(c, PROVEN_ERR_INVALID_FORMAT, "not a PKCS#12 (version 3) file");
    }
    rp_der_span_t as = rp_der_inside(&auth);
    if (!rp_der_get(&as, RP_DER_OID, &type) || !rp_der_oid_is(&type, O_DATA, sizeof O_DATA) || !rp_der_get(&as, RP_DER_CTX0, &wrap)) {
        return fail(c, PROVEN_ERR_UNSUPPORTED, "a PKCS#12 file protected with public keys instead of a password");
    }
    rp_der_span_t ws = rp_der_inside(&wrap);
    if (!rp_der_get(&ws, RP_DER_OCTET_STRING, &oct)) return fail(c, PROVEN_ERR_INVALID_FORMAT, "a malformed PKCS#12 file");
    // MacData: checked before anything is decrypted.
    if (!rp_der_get(&ps, RP_DER_SEQUENCE, &mac)) return fail(c, PROVEN_ERR_UNSUPPORTED, "a PKCS#12 file without a MAC");
    rp_der_span_t ms = rp_der_inside(&mac);
    rp_der_t digest_info, alg, alg_oid, digest, salt, iter;
    if (!rp_der_get(&ms, RP_DER_SEQUENCE, &digest_info) || !rp_der_get(&ms, RP_DER_OCTET_STRING, &salt)) {
        return fail(c, PROVEN_ERR_INVALID_FORMAT, "a malformed PKCS#12 MAC");
    }
    uint64_t iterations = 1;
    if (rp_der_get(&ms, RP_DER_INTEGER, &iter) && !rp_der_small(&iter, &iterations)) return fail(c, PROVEN_ERR_INVALID_FORMAT, "a malformed PKCS#12 MAC");
    rp_der_span_t ds = rp_der_inside(&digest_info);
    if (!rp_der_get(&ds, RP_DER_SEQUENCE, &alg) || !rp_der_get(&ds, RP_DER_OCTET_STRING, &digest)) return fail(c, PROVEN_ERR_INVALID_FORMAT, "a malformed PKCS#12 MAC");
    rp_der_span_t als = rp_der_inside(&alg);
    rp_hash_alg_t h;
    if (!rp_der_get(&als, RP_DER_OID, &alg_oid)) return fail(c, PROVEN_ERR_INVALID_FORMAT, "a malformed PKCS#12 MAC");
    if (rp_der_oid_is(&alg_oid, O_SHA256, sizeof O_SHA256)) h = RP_HASH_SHA256;
    else if (rp_der_oid_is(&alg_oid, O_SHA384, sizeof O_SHA384)) h = RP_HASH_SHA384;
    else if (rp_der_oid_is(&alg_oid, O_SHA512, sizeof O_SHA512)) h = RP_HASH_SHA512;
    else return fail(c, PROVEN_ERR_UNSUPPORTED, "a PKCS#12 MAC with SHA-1 or another old hash; export it again with SHA-256");
    if (iterations < 1 || iterations > MAX_ITER) return fail(c, PROVEN_ERR_UNSUPPORTED, "a PKCS#12 MAC iteration count of 0 or above 1,000,000");
    if (digest.val.n != rp_hash_size(h) || salt.val.n > 1024) return fail(c, PROVEN_ERR_INVALID_FORMAT, "a malformed PKCS#12 MAC");
    // The password as a BMPString with its closing NUL (an absent password is empty).
    uint8_t bmp[2 * 512 + 2];
    size_t bmp_len = 0;
    if (c->pass) {
        proven_u16 w[512];
        rp_text_result_t r = rp_utf8_to_utf16(c->pass, c->pass_len, w, 512);
        if (r.err != PROVEN_OK) return fail(c, PROVEN_ERR_INVALID_ARG, "the password is not valid UTF-8 or longer than 512 characters");
        for (size_t i = 0; i < r.units; ++i) {
            bmp[2 * i] = (uint8_t)(w[i] >> 8);
            bmp[2 * i + 1] = (uint8_t)w[i];
        }
        bmp_len = 2 * r.units + 2;
        bmp[bmp_len - 2] = bmp[bmp_len - 1] = 0;
        rp_wipe(w, sizeof w);
    }
    uint8_t key[RP_HASH_MAX], got[RP_HASH_MAX];
    size_t hl = rp_hash_size(h);
    p12_kdf(h, bmp, bmp_len, salt.val.p, salt.val.n, (uint32_t)iterations, key, hl);
    rp_hmac(h, key, hl, oct.val.p, oct.val.n, got);
    rp_wipe(key, sizeof key);
    rp_wipe(bmp, sizeof bmp);
    if (!rp_ct_equal(got, digest.val.p, hl)) return fail(c, PROVEN_ERR_PERMISSION, "wrong password (the PKCS#12 MAC does not match)");
    // AuthenticatedSafe.
    rp_der_span_t sa = { oct.val.p, oct.val.n };
    rp_der_t list, ci;
    if (!rp_der_get(&sa, RP_DER_SEQUENCE, &list)) return fail(c, PROVEN_ERR_INVALID_FORMAT, "a malformed PKCS#12 file");
    rp_der_span_t ls = rp_der_inside(&list);
    while (rp_der_get(&ls, RP_DER_SEQUENCE, &ci)) {
        if (!auth_content(c, &ci)) return false;
    }
    if (ls.n) return fail(c, PROVEN_ERR_INVALID_FORMAT, "a malformed PKCS#12 file");
    return true;
}

// ---- PEM -----------------------------------------------------------------------------------------

static int b64(uint8_t ch) {
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    if (ch == '+') return 62;
    if (ch == '/') return 63;
    return -1;
}

// Every "-----BEGIN <label>-----" block: decoded and handed to `each`. Data outside blocks is
// ignored (RFC 7468 explanatory text).
static bool pem_blocks(ctx_t *c, const uint8_t *text, size_t len, bool (*each)(ctx_t *, const char *label, const uint8_t *, size_t)) {
    size_t i = 0, blocks = 0;
    while (i < len) {
        const uint8_t *b = memchr(text + i, '-', len - i);
        if (b == NULL) break;
        i = (size_t)(b - text);
        if (len - i < 11 || memcmp(text + i, "-----BEGIN ", 11) != 0) {
            ++i;
            continue;
        }
        size_t ls = i + 11, le = ls;
        while (le < len && text[le] != '-' && text[le] != '\n') ++le;
        if (le + 5 > len || memcmp(text + le, "-----", 5) != 0 || le - ls > 40) return fail(c, PROVEN_ERR_INVALID_FORMAT, "a malformed PEM header");
        char label[48];
        memcpy(label, text + ls, le - ls);
        label[le - ls] = 0;
        size_t body = le + 5;
        char end_line[64];
        int en = snprintf(end_line, sizeof end_line, "-----END %s-----", label);
        const uint8_t *end = NULL;
        for (size_t k = body; k + (size_t)en <= len && end == NULL; ++k) {
            if (text[k] == '-' && memcmp(text + k, end_line, (size_t)en) == 0) end = text + k;
        }
        if (end == NULL) return fail(c, PROVEN_ERR_INVALID_FORMAT, "a PEM block without its END line");
        size_t blen = (size_t)(end - (text + body));
        uint8_t *der = rp_mem_alloc(c->alloc, blen + 1, 1);
        if (der == NULL) return fail(c, PROVEN_ERR_NOMEM, "out of memory");
        size_t n = 0;
        uint32_t acc = 0;
        int bits = 0, pad = 0;
        bool ok = true;
        for (size_t k = body; k < body + blen && ok; ++k) {
            uint8_t ch = text[k];
            if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') continue;
            if (ch == '=') {
                ++pad;
                continue;
            }
            int v = b64(ch);
            if (v < 0 || pad) ok = false;
            acc = (acc << 6) | (uint32_t)v;
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                der[n++] = (uint8_t)(acc >> bits);
            }
        }
        ok = ok && pad <= 2;
        ok = ok && each(c, label, der, n);
        rp_wipe(der, blen + 1);
        rp_mem_free(c->alloc, der);
        if (!ok) return c->err == PROVEN_OK ? fail(c, PROVEN_ERR_INVALID_FORMAT, "a malformed PEM block") : false;
        ++blocks;
        i = (size_t)(end - text) + (size_t)en;
    }
    if (blocks == 0) return fail(c, PROVEN_ERR_INVALID_FORMAT, "no PEM blocks in the file");
    return true;
}

static bool pem_one(ctx_t *c, const char *label, const uint8_t *der, size_t n) {
    if (strcmp(label, "CERTIFICATE") == 0) return add_cert(c, der, n);
    if (strcmp(label, "PRIVATE KEY") == 0) return take_pkcs8(c, der, n);
    if (strcmp(label, "ENCRYPTED PRIVATE KEY") == 0) return take_encrypted_pkcs8(c, der, n);
    if (strcmp(label, "RSA PRIVATE KEY") == 0) {
        return fail(c, PROVEN_ERR_UNSUPPORTED, "an old-style RSA PRIVATE KEY block; convert it to PKCS#8 (BEGIN PRIVATE KEY or ENCRYPTED PRIVATE KEY)");
    }
    return true;        // other blocks (parameters, CRLs) are not needed
}

static bool pem_certs_only(ctx_t *c, const char *label, const uint8_t *der, size_t n) {
    if (strcmp(label, "CERTIFICATE") == 0) return add_cert(c, der, n);
    return true;
}

// ---- entry ---------------------------------------------------------------------------------------

static bool has_pem(const uint8_t *data, size_t len) {
    static const char tag[] = "-----BEGIN ";
    for (size_t i = 0; i + sizeof tag - 1 <= len; ++i) {
        if (data[i] == '-' && memcmp(data + i, tag, sizeof tag - 1) == 0) return true;
    }
    return false;
}

proven_err_t rp_keyfile_load(proven_allocator_t alloc, const uint8_t *data, size_t len, const uint8_t *pass, size_t pass_len,
                             rp_keyfile_t *out, const char **why) {
    const char *dummy;
    if (why == NULL) why = &dummy;
    *why = NULL;
    memset(out, 0, sizeof *out);
    out->alloc = alloc;
    if (len > MAX_FILE) {
        *why = "the key file is larger than 4 MiB";
        return PROVEN_ERR_OUT_OF_BOUNDS;
    }
    ctx_t c = { alloc, pass, pass_len, out, why, PROVEN_OK };
    bool ok;
    if (has_pem(data, len)) {
        ok = pem_blocks(&c, data, len, pem_one);
    } else {
        // Binary: a PKCS#12 file (possibly BER) or a DER PKCS#8 key.
        uint8_t *der = NULL;
        size_t der_len = 0;
        rp_limits_t lim = rp_limits_default();
        if (rp_ber_to_der(alloc, data, len, lim.max_depth, MAX_FILE, &der, &der_len) != PROVEN_OK) {
            *why = "not a PEM, PKCS#8 or PKCS#12 file";
            return PROVEN_ERR_INVALID_FORMAT;
        }
        rp_der_span_t s = { der, der_len };
        rp_der_t top, first;
        bool pfx = false;
        if (rp_der_get(&s, RP_DER_SEQUENCE, &top)) {
            rp_der_span_t ts = rp_der_inside(&top);
            uint64_t v;
            pfx = rp_der_get(&ts, RP_DER_INTEGER, &first) && rp_der_small(&first, &v) && v == 3;
        }
        if (pfx) ok = load_pfx(&c, der, der_len);
        else if (memcmp(der, data, der_len < len ? der_len : len) == 0 && der_len == len) {
            // A DER key: plain PrivateKeyInfo starts with INTEGER 0/1, encrypted with a SEQUENCE.
            rp_der_span_t ks = { der, der_len };
            rp_der_t k, k0;
            rp_der_span_t kin = rp_der_get(&ks, RP_DER_SEQUENCE, &k) ? rp_der_inside(&k) : (rp_der_span_t){ 0 };
            ok = rp_der_peek(&kin, &k0.tag) && k0.tag == RP_DER_SEQUENCE ? take_encrypted_pkcs8(&c, der, der_len) : take_pkcs8(&c, der, der_len);
        } else {
            ok = fail(&c, PROVEN_ERR_INVALID_FORMAT, "a BER-encoded file that is not PKCS#12");
        }
        rp_wipe(der, der_len);
        rp_mem_free(alloc, der);
    }
    if (ok && out->key_der == NULL) ok = fail(&c, PROVEN_ERR_INVALID_FORMAT, "the file holds no private key");
    if (!ok) {
        rp_keyfile_free(out);
        return c.err == PROVEN_OK ? PROVEN_ERR_INVALID_FORMAT : c.err;
    }
    return PROVEN_OK;
}

proven_err_t rp_keyfile_add_certs(rp_keyfile_t *kf, const uint8_t *data, size_t len, const char **why) {
    const char *dummy;
    if (why == NULL) why = &dummy;
    *why = NULL;
    ctx_t c = { kf->alloc, NULL, 0, kf, why, PROVEN_OK };
    bool ok = has_pem(data, len) ? pem_blocks(&c, data, len, pem_certs_only) : add_cert(&c, data, len);
    return ok ? PROVEN_OK : c.err;
}

void rp_keyfile_free(rp_keyfile_t *kf) {
    if (kf->key_der) {
        rp_wipe(kf->key_der, kf->key_len);
        rp_mem_free(kf->alloc, kf->key_der);
    }
    for (size_t i = 0; i < kf->cert_count; ++i) rp_mem_free(kf->alloc, kf->certs[i]);
    rp_mem_free(kf->alloc, kf->certs);
    rp_mem_free(kf->alloc, kf->cert_len);
    memset(kf, 0, sizeof *kf);
}

int rp_keyfile_leaf(const rp_keyfile_t *kf) {
    for (size_t i = 0; i < kf->cert_count; ++i) {
        rp_cert_t c;
        if (!rp_cert_parse(kf->certs[i], kf->cert_len[i], &c, NULL) || !c.rsa) continue;
        const uint8_t *n = kf->rsa.n, *e = kf->rsa.e;
        size_t nl = kf->rsa.n_len, el = kf->rsa.e_len;
        if (c.rsa_n.n == nl && c.rsa_e.n == el && memcmp(c.rsa_n.p, n, nl) == 0 && memcmp(c.rsa_e.p, e, el) == 0) return (int)i;
    }
    return -1;
}
