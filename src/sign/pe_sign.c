// src/sign/pe_sign.c - Authenticode for PE files (include/rubrapack/sign.h), from Microsoft's
// "Windows Authenticode Portable Executable Signature Format" and the PE/COFF specification.

#include "rubrapack/in.h"
#include "rubrapack/le.h"
#include "rubrapack/limits.h"
#include "rubrapack/lang.h"
#include "rubrapack/mem.h"
#include "rubrapack/sign.h"

#include <string.h>

typedef struct {
    size_t   checksum;      // offset of CheckSum
    size_t   certdir;       // offset of the certificate table's directory entry
    size_t   headers;       // SizeOfHeaders
    size_t   sections;      // offset of the section table
    uint16_t nsections;
    uint32_t cert_off, cert_size;
} layout_t;

static bool layout(const uint8_t *pe, size_t len, layout_t *l, const char **why) {
    memset(l, 0, sizeof *l);
    rp_in_t in = rp_in(pe, len);
    if (len < 0x40 || rp_in8(&in, 0) != 'M' || rp_in8(&in, 1) != 'Z') {
        *why = TR_N("not a PE file (no MZ header)");
        return false;
    }
    uint32_t nt = rp_in32(&in, 0x3C);
    const uint8_t *sig = rp_range_ok(nt, 24, len) ? rp_in_bytes(&in, nt, 4) : NULL;
    if (sig == NULL || memcmp(sig, "PE\0\0", 4) != 0) {
        *why = TR_N("not a PE file (no PE header)");
        return false;
    }
    l->nsections = rp_in16(&in, (size_t)nt + 6);
    uint16_t opt_size = rp_in16(&in, (size_t)nt + 20);
    size_t opt = nt + 24;
    if (!rp_range_ok(opt, opt_size, len) || opt_size < 2) {
        *why = TR_N("a damaged PE optional header");
        return false;
    }
    rp_in_t oh = rp_in_sub(&in, opt, opt_size);
    uint16_t magic = rp_in16(&oh, 0);
    size_t nrva_off = magic == 0x20B ? 108 : magic == 0x10B ? 92 : 0, dd_off = magic == 0x20B ? 112 : 96;
    if (nrva_off == 0 || opt_size < dd_off + 5 * 8 || rp_in32(&oh, nrva_off) < 5) {
        *why = TR_N("a PE file without a certificate table entry");
        return false;
    }
    l->checksum = opt + 64;
    l->headers = rp_in32(&oh, 60);
    l->certdir = opt + dd_off + 4 * 8;
    l->cert_off = rp_in32(&oh, dd_off + 4 * 8);
    l->cert_size = rp_in32(&oh, dd_off + 4 * 8 + 4);
    if (in.bad || oh.bad) {
        *why = TR_N("a damaged PE optional header");
        return false;
    }
    l->sections = opt + opt_size;
    if (!rp_range_ok(l->sections, (uint64_t)l->nsections * 40, len) || l->headers > len || l->headers < l->sections + (size_t)l->nsections * 40) {
        *why = TR_N("a damaged PE section table");
        return false;
    }
    if (l->cert_size && ((uint64_t)l->cert_off + l->cert_size != len || l->cert_off < l->headers)) {
        *why = TR_N("the certificate table is not at the end of the file");
        return false;
    }
    return true;
}

// The Authenticode digest stream of a PE file, fed to `feed`: the headers without CheckSum and the
// certificate table entry, the sections in file order, the data after them up to the certificate
// table. *unsigned_len is the file's length when it has no certificate table (0 when it has one).
static bool pe_stream(const uint8_t *pe, size_t len, void (*feed)(void *, const void *, size_t), void *ctx, size_t *unsigned_len,
                      const char **why) {
    layout_t l;
    if (!layout(pe, len, &l, why)) return false;
    rp_in_t in = rp_in(pe, len);
    // The ranges hashed, each through the view: layout() has placed them, the view keeps them in.
    const uint8_t *h0 = rp_in_bytes(&in, 0, l.checksum), *h1 = rp_in_bytes(&in, l.checksum + 4, l.certdir - (l.checksum + 4)),
                  *h2 = rp_in_bytes(&in, l.certdir + 8, l.headers - (l.certdir + 8));
    if (h0 == NULL || h1 == NULL || h2 == NULL) {
        *why = TR_N("a damaged PE section table");
        return false;
    }
    feed(ctx, h0, l.checksum);
    feed(ctx, h1, l.certdir - (l.checksum + 4));
    feed(ctx, h2, l.headers - (l.certdir + 8));
    uint32_t order[96];
    size_t n = 0;
    if (l.nsections > 96) {
        *why = TR_N("more than 96 sections");
        return false;
    }
    rp_in_t st = rp_in_sub(&in, l.sections, 40u * (size_t)l.nsections);
    for (uint16_t k = 0; k < l.nsections; ++k) {
        if (rp_in32(&st, 40u * k + 16) == 0) continue;
        order[n++] = k;
    }
    for (size_t i = 1; i < n; ++i) {
        for (size_t j = i; j > 0 && rp_in32(&st, 40u * order[j - 1] + 20) > rp_in32(&st, 40u * order[j] + 20); --j) {
            uint32_t t = order[j];
            order[j] = order[j - 1];
            order[j - 1] = t;
        }
    }
    uint64_t hashed = l.headers;
    size_t end = l.cert_size ? l.cert_off : len;
    for (size_t i = 0; i < n; ++i) {
        uint32_t size = rp_in32(&st, 40u * order[i] + 16), off = rp_in32(&st, 40u * order[i] + 20);
        const uint8_t *body = rp_range_ok(off, size, end) ? rp_in_bytes(&in, off, size) : NULL;
        if (body == NULL || st.bad) {
            *why = TR_N("a section lies outside the file");
            return false;
        }
        feed(ctx, body, size);
        hashed += size;
    }
    if (hashed < end) {
        const uint8_t *tail = rp_in_bytes(&in, (size_t)hashed, end - (size_t)hashed);
        if (tail == NULL) {
            *why = TR_N("a section lies outside the file");
            return false;
        }
        feed(ctx, tail, end - (size_t)hashed);
    }
    *unsigned_len = l.cert_size ? 0 : len;
    return true;
}

static void feed_hash(void *ctx, const void *p, size_t n) { rp_hash_update(ctx, p, n); }

bool rp_pe_digest(const uint8_t *pe, size_t len, rp_hash_alg_t alg, uint8_t *digest, const char **why) {
    rp_hash_t h;
    size_t ul;
    rp_hash_init(&h, alg);
    if (!pe_stream(pe, len, feed_hash, &h, &ul, why)) return false;
    rp_hash_final(&h, digest);
    return true;
}

typedef struct {
    rp_hash_t h256;
    rp_sha1_t h1;
} both_t;

static void feed_both(void *ctx, const void *p, size_t n) {
    both_t *b = ctx;
    rp_hash_update(&b->h256, p, n);
    rp_sha1_update(&b->h1, p, n);
}

bool rp_pe_catalog_hashes(const uint8_t *pe, size_t len, uint8_t sha1[20], uint8_t sha256[32], const char **why) {
    both_t b;
    size_t ul;
    rp_hash_init(&b.h256, RP_HASH_SHA256);
    rp_sha1_init(&b.h1);
    if (!pe_stream(pe, len, feed_both, &b, &ul, why)) return false;
    // An unsigned file is hashed as if padded with zeros to a multiple of 8 bytes, as it would be
    // before a signature is added (Windows' CryptCATAdminCalcHashFromFileHandle2; format notes F9).
    static const uint8_t zero[8] = { 0 };
    if (ul % 8) feed_both(&b, zero, 8 - ul % 8);
    rp_hash_final(&b.h256, sha256);
    rp_sha1_final(&b.h1, sha1);
    return true;
}

// PE checksum: 16-bit one's-complement-style sum of the file (CheckSum taken as 0) plus its length.
static uint32_t pe_checksum(const uint8_t *pe, size_t len, size_t cs) {
    rp_in_t in = rp_in(pe, len);
    uint64_t sum = 0;
    for (size_t i = 0; i < len; i += 2) {
        if (i >= cs && i < cs + 4) continue;
        uint32_t w = i + 1 < len ? rp_in16(&in, i) : rp_in8(&in, i);
        sum += w;
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint32_t)(sum + len);
}

// SpcAttributeTypeAndOptionalValue for a PE image, as Windows writes it: SPC_PE_IMAGE_DATA with
// SpcPeImageData { flags '' (BIT STRING, no bits), file [0] { [2] { [0] "" } } }.
static const uint8_t PE_DATA[] = { 0x30, 0x17, 0x06, 0x0A, 0x2B, 0x06, 0x01, 0x04, 0x01, 0x82, 0x37, 0x02, 0x01, 0x0F,
                                   0x30, 0x09, 0x03, 0x01, 0x00, 0xA0, 0x04, 0xA2, 0x02, 0x80, 0x00 };

proven_err_t rp_pe_sign(proven_allocator_t alloc, const uint8_t *pe, size_t len, const rp_keyfile_t *kf, int64_t now,
                        const rp_timestamper_t *ts, uint8_t **out, size_t *out_len, const char **why) {
    layout_t l;
    if (!layout(pe, len, &l, why)) return PROVEN_ERR_INVALID_FORMAT;
    if (l.cert_off || l.cert_size) {
        *why = TR_N("the file is signed already; rubrapack does not sign twice or replace a signature");
        return PROVEN_ERR_INVALID_STATE;
    }
    int leaf;
    if (!rp_sign_check_key(kf, now, &leaf, why)) return PROVEN_ERR_INVALID_ARG;
    size_t padded = (len + 7) & ~(size_t)7;
    if (padded > 0xFFFFFFF0u - 65536) {
        *why = TR_N("the file is too large to sign");
        return PROVEN_ERR_OUT_OF_BOUNDS;
    }
    uint8_t *work = rp_mem_alloc(alloc, padded, 1);
    if (work == NULL) return PROVEN_ERR_NOMEM;
    memcpy(work, pe, len);
    memset(work + len, 0, padded - len);        // the padding is hashed as data after the sections
    uint8_t digest[RP_HASH_MAX];
    if (!rp_pe_digest(work, padded, RP_HASH_SHA256, digest, why)) {
        rp_mem_free(alloc, work);
        return PROVEN_ERR_INVALID_FORMAT;
    }
    uint8_t *p7 = NULL;
    size_t p7_len = 0;
    proven_err_t err = rp_authenticode_build(alloc, kf, leaf, RP_HASH_SHA256, PE_DATA, sizeof PE_DATA, digest, ts, &p7, &p7_len, why);
    if (err != PROVEN_OK) {
        rp_mem_free(alloc, work);
        return err;
    }
    size_t wlen = (8 + p7_len + 7) & ~(size_t)7;     // WIN_CERTIFICATE, zero-filled to 8 bytes
    uint8_t *o = rp_mem_alloc(alloc, padded + wlen, 1);
    if (o == NULL) {
        rp_mem_free(alloc, work);
        rp_mem_free(alloc, p7);
        return PROVEN_ERR_NOMEM;
    }
    memcpy(o, work, padded);
    memset(o + padded, 0, wlen);
    rp_wr32(o + padded, (uint32_t)wlen);
    o[padded + 4] = 0x00;                            // wRevision 0x0200
    o[padded + 5] = 0x02;
    o[padded + 6] = 0x02;                            // wCertificateType PKCS_SIGNED_DATA
    o[padded + 7] = 0x00;
    memcpy(o + padded + 8, p7, p7_len);
    rp_wr32(o + l.certdir, (uint32_t)padded);
    rp_wr32(o + l.certdir + 4, (uint32_t)wlen);
    rp_wr32(o + l.checksum, pe_checksum(o, padded + wlen, l.checksum));
    rp_mem_free(alloc, work);
    rp_mem_free(alloc, p7);
    *out = o;
    *out_len = padded + wlen;
    return PROVEN_OK;
}

void rp_pe_verify(const uint8_t *pe, size_t len, rp_authenticode_check_t *r, const char **why) {
    memset(r, 0, sizeof *r);
    layout_t l;
    if (!layout(pe, len, &l, why)) return;
    if (l.cert_size < 8) {
        *why = TR_N("the file is not signed");
        return;
    }
    rp_in_t in = rp_in(pe, len);
    rp_in_t w = rp_in_sub(&in, l.cert_off, l.cert_size);
    uint32_t wl = rp_in32(&w, 0);
    if (w.bad || wl < 8 || wl > l.cert_size || rp_in16(&w, 4) != 0x0200 || rp_in16(&w, 6) != 0x0002) {
        *why = TR_N("the certificate table does not hold an Authenticode signature");
        return;
    }
    // The PKCS#7 inside ends where its DER says; after it only the zeros that pad the table to
    // eight bytes. Bytes there would be in a signed file without being signed (CVE-2013-3900:
    // Windows refuses them when its padding check is on).
    rp_in_t body = rp_in_sub(&w, 8, wl - 8);
    rp_der_span_t s = { body.p, body.n };
    rp_der_t ci;
    if (!rp_der_read(&s, &ci)) {
        *why = TR_N("the signature is not DER");
        return;
    }
    size_t used = 8 + ci.whole.n, rest = l.cert_size - used;
    bool zeros = rest < 8;
    for (size_t i = 0; zeros && i < rest; ++i) zeros = rp_in8(&w, used + i) == 0;
    if (!zeros) {
        *why = "bytes follow the signature that it does not cover";
        return;
    }
    uint8_t digest[RP_HASH_MAX];
    rp_authenticode_verify(ci.whole.p, ci.whole.n, NULL, r, why);
    if (!r->parsed) return;
    if (!rp_pe_digest(pe, len, r->alg, digest, why)) return;
    const char *vwhy = NULL;
    rp_authenticode_check_t again;
    rp_authenticode_verify(ci.whole.p, ci.whole.n, digest, &again, &vwhy);
    *r = again;
    if (!r->digest_ok) *why = TR_N("the file was changed after it was signed (its digest does not match)");
    else *why = vwhy;
}
