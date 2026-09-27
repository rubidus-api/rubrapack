// src/sign/pe_sign.c - Authenticode for PE files (include/rubrapack/sign.h), from Microsoft's
// "Windows Authenticode Portable Executable Signature Format" and the PE/COFF specification.

#include "rubrapack/limits.h"
#include "rubrapack/mem.h"
#include "rubrapack/sign.h"

#include <string.h>

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

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
    if (len < 0x40 || pe[0] != 'M' || pe[1] != 'Z') {
        *why = "not a PE file (no MZ header)";
        return false;
    }
    uint32_t nt = rd32(pe + 0x3C);
    if (!rp_range_ok(nt, 24, len) || memcmp(pe + nt, "PE\0\0", 4) != 0) {
        *why = "not a PE file (no PE header)";
        return false;
    }
    l->nsections = rd16(pe + nt + 6);
    uint16_t opt_size = rd16(pe + nt + 20);
    size_t opt = nt + 24;
    if (!rp_range_ok(opt, opt_size, len) || opt_size < 2) {
        *why = "a damaged PE optional header";
        return false;
    }
    uint16_t magic = rd16(pe + opt);
    size_t nrva_off = magic == 0x20B ? 108 : magic == 0x10B ? 92 : 0, dd_off = magic == 0x20B ? 112 : 96;
    if (nrva_off == 0 || opt_size < dd_off + 5 * 8 || rd32(pe + opt + nrva_off) < 5) {
        *why = "a PE file without a certificate table entry";
        return false;
    }
    l->checksum = opt + 64;
    l->headers = rd32(pe + opt + 60);
    l->certdir = opt + dd_off + 4 * 8;
    l->cert_off = rd32(pe + l->certdir);
    l->cert_size = rd32(pe + l->certdir + 4);
    l->sections = opt + opt_size;
    if (!rp_range_ok(l->sections, (uint64_t)l->nsections * 40, len) || l->headers > len || l->headers < l->sections + (size_t)l->nsections * 40) {
        *why = "a damaged PE section table";
        return false;
    }
    if (l->cert_size && ((uint64_t)l->cert_off + l->cert_size != len || l->cert_off < l->headers)) {
        *why = "the certificate table is not at the end of the file";
        return false;
    }
    return true;
}

bool rp_pe_digest(const uint8_t *pe, size_t len, rp_hash_alg_t alg, uint8_t *digest, const char **why) {
    layout_t l;
    if (!layout(pe, len, &l, why)) return false;
    rp_hash_t h;
    rp_hash_init(&h, alg);
    // Headers without CheckSum and the certificate table entry.
    rp_hash_update(&h, pe, l.checksum);
    rp_hash_update(&h, pe + l.checksum + 4, l.certdir - (l.checksum + 4));
    rp_hash_update(&h, pe + l.certdir + 8, l.headers - (l.certdir + 8));
    // Sections in file order.
    uint32_t order[96];
    size_t n = 0;
    if (l.nsections > 96) {
        *why = "more than 96 sections";
        return false;
    }
    for (uint16_t k = 0; k < l.nsections; ++k) {
        const uint8_t *s = pe + l.sections + 40u * k;
        if (rd32(s + 16) == 0) continue;
        order[n++] = k;
    }
    for (size_t i = 1; i < n; ++i) {
        for (size_t j = i; j > 0 && rd32(pe + l.sections + 40u * order[j - 1] + 20) > rd32(pe + l.sections + 40u * order[j] + 20); --j) {
            uint32_t t = order[j];
            order[j] = order[j - 1];
            order[j - 1] = t;
        }
    }
    uint64_t hashed = l.headers;
    size_t end = l.cert_size ? l.cert_off : len;
    for (size_t i = 0; i < n; ++i) {
        const uint8_t *s = pe + l.sections + 40u * order[i];
        uint32_t size = rd32(s + 16), off = rd32(s + 20);
        if (!rp_range_ok(off, size, end)) {
            *why = "a section lies outside the file";
            return false;
        }
        rp_hash_update(&h, pe + off, size);
        hashed += size;
    }
    // The data after the sections, up to the certificate table.
    if (hashed < end) rp_hash_update(&h, pe + hashed, end - (size_t)hashed);
    rp_hash_final(&h, digest);
    return true;
}

// PE checksum: 16-bit one's-complement-style sum of the file (CheckSum taken as 0) plus its length.
static uint32_t pe_checksum(const uint8_t *pe, size_t len, size_t cs) {
    uint64_t sum = 0;
    for (size_t i = 0; i < len; i += 2) {
        if (i >= cs && i < cs + 4) continue;
        uint32_t w = i + 1 < len ? rd16(pe + i) : pe[i];
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
        *why = "the file is signed already; rubrapack does not sign twice or replace a signature";
        return PROVEN_ERR_INVALID_STATE;
    }
    int leaf;
    if (!rp_sign_check_key(kf, now, &leaf, why)) return PROVEN_ERR_INVALID_ARG;
    size_t padded = (len + 7) & ~(size_t)7;
    if (padded > 0xFFFFFFF0u - 65536) {
        *why = "the file is too large to sign";
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
    wr32(o + padded, (uint32_t)wlen);
    o[padded + 4] = 0x00;                            // wRevision 0x0200
    o[padded + 5] = 0x02;
    o[padded + 6] = 0x02;                            // wCertificateType PKCS_SIGNED_DATA
    o[padded + 7] = 0x00;
    memcpy(o + padded + 8, p7, p7_len);
    wr32(o + l.certdir, (uint32_t)padded);
    wr32(o + l.certdir + 4, (uint32_t)wlen);
    wr32(o + l.checksum, pe_checksum(o, padded + wlen, l.checksum));
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
        *why = "the file is not signed";
        return;
    }
    const uint8_t *w = pe + l.cert_off;
    uint32_t wl = rd32(w);
    if (wl < 8 || wl > l.cert_size || rd16(w + 4) != 0x0200 || rd16(w + 6) != 0x0002) {
        *why = "the certificate table does not hold an Authenticode signature";
        return;
    }
    // The PKCS#7 inside ends where its DER says; the rest is padding.
    rp_der_span_t s = { w + 8, wl - 8 };
    rp_der_t ci;
    if (!rp_der_read(&s, &ci)) {
        *why = "the signature is not DER";
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
    if (!r->digest_ok) *why = "the file was changed after it was signed (its digest does not match)";
    else *why = vwhy;
}
