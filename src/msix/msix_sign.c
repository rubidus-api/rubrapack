// src/msix/msix_sign.c - signing and checking MSIX packages and bundles (RFC-0011 P9a), as Windows'
// AppX SIP does (docs in manual/formats/msix.md "Signatures"; worked out against mssign32!SignerSignEx2):
// AppxSignature.p7x = "PKCX" + an Authenticode SignedData whose indirect data is SpcSipInfo (the
// package or bundle SIP GUID) with the digest "APPX" + AXPC (the ZIP up to the signature's local
// header), AXCD (the central directory and end records without the signature entry), AXCT
// ([Content_Types].xml), AXBM (AppxBlockMap.xml) and, when a package has one, AXCI
// (AppxMetadata/CodeIntegrity.cat); each a SHA-256. A bundle's packages are signed first.

#include "rubrapack/buf.h"
#include "rubrapack/crypto.h"
#include "rubrapack/deflate.h"
#include "rubrapack/der.h"
#include "rubrapack/mem.h"
#include "rubrapack/msix.h"
#include "rubrapack/pki.h"
#include "rubrapack/sign.h"
#include "rubrapack/zip.h"

#include "proven/hash.h"

#include <stdio.h>
#include <string.h>

enum { LEVEL = 6 };

static const uint8_t O_SPC_SIPINFO[] = { 0x2B, 0x06, 0x01, 0x04, 0x01, 0x82, 0x37, 0x02, 0x01, 0x1E };
static const uint8_t GUID_PACKAGE[16] = { 0x4B, 0xDF, 0xC5, 0x0A, 0x07, 0xCE, 0xE2, 0x4D, 0xB7, 0x6E, 0x23, 0xC8, 0x39, 0xA0, 0x9F, 0xD1 };
static const uint8_t GUID_BUNDLE[16] = { 0xB3, 0x58, 0x5F, 0x0F, 0xDE, 0xAA, 0x9A, 0x4B, 0xA4, 0x34, 0x95, 0x74, 0x2D, 0x92, 0xEC, 0xEB };
static const char SIG_NAME[] = "AppxSignature.p7x", CI_NAME[] = "AppxMetadata/CodeIntegrity.cat";
static const char SIG_OVERRIDE[] = "<Override PartName=\"/AppxSignature.p7x\" ContentType=\"application/vnd.ms-appx.signature\" />";

// SpcAttributeTypeAndOptionalValue { SpcSipInfo, SEQUENCE { 0x01010000, GUID, 0, 0, 0, 0, 0 } }.
static void sip_info(rp_buf_t *out, bool bundle) {
    rp_buf_t a = rp_buf_new(out->alloc, 256), v = rp_buf_new(out->alloc, 256);
    rp_der_put_oid(&a, O_SPC_SIPINFO, sizeof O_SPC_SIPINFO);
    rp_der_put_small(&v, 0x01010000);
    rp_der_put(&v, RP_DER_OCTET_STRING, bundle ? GUID_BUNDLE : GUID_PACKAGE, 16);
    for (int i = 0; i < 5; ++i) rp_der_put_small(&v, 0);
    rp_der_wrap(&a, RP_DER_SEQUENCE, &v);
    rp_der_wrap(out, RP_DER_SEQUENCE, &a);
}

// ---- the publisher (RFC-0011 W3) ------------------------------------------------------------------

static const struct {
    uint8_t     oid[10];
    size_t      n;
    const char *name;
} short_names[] = {
    { { 0x55, 0x04, 0x03 }, 3, "CN" },   { { 0x55, 0x04, 0x06 }, 3, "C" },     { { 0x55, 0x04, 0x07 }, 3, "L" },
    { { 0x55, 0x04, 0x08 }, 3, "S" },    { { 0x55, 0x04, 0x09 }, 3, "STREET" }, { { 0x55, 0x04, 0x0A }, 3, "O" },
    { { 0x55, 0x04, 0x0B }, 3, "OU" },   { { 0x55, 0x04, 0x0C }, 3, "T" },     { { 0x55, 0x04, 0x2A }, 3, "G" },
    { { 0x55, 0x04, 0x2B }, 3, "I" },    { { 0x55, 0x04, 0x04 }, 3, "SN" },    { { 0x55, 0x04, 0x05 }, 3, "SERIALNUMBER" },
    { { 0x55, 0x04, 0x11 }, 3, "PostalCode" },
    { { 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x09, 0x01 }, 9, "E" },
    { { 0x09, 0x92, 0x26, 0x89, 0x93, 0xF2, 0x2C, 0x64, 0x01, 0x19 }, 10, "DC" },
};

// One attribute value as text; false for a string type rubrapack does not turn into UTF-8.
static bool value_text(const rp_der_t *v, rp_buf_t *out) {
    switch (v->tag) {
    case 0x0C: case 0x13: case 0x16: case 0x14:          // UTF8String, PrintableString, IA5String, T61String
        rp_buf_put(out, v->val.p, v->val.n);
        return true;
    case 0x1E:                                           // BMPString: UTF-16BE
        for (size_t i = 0; i + 1 < v->val.n; i += 2) {
            unsigned c = (unsigned)v->val.p[i] << 8 | v->val.p[i + 1];
            if (c >= 0xD800 && c <= 0xDFFF) return false;
            if (c < 0x80) rp_buf_byte(out, (uint8_t)c);
            else if (c < 0x800) rp_buf_byte(out, (uint8_t)(0xC0 | c >> 6)), rp_buf_byte(out, (uint8_t)(0x80 | (c & 63)));
            else rp_buf_byte(out, (uint8_t)(0xE0 | c >> 12)), rp_buf_byte(out, (uint8_t)(0x80 | (c >> 6 & 63))), rp_buf_byte(out, (uint8_t)(0x80 | (c & 63)));
        }
        return true;
    default:
        return false;
    }
}

// The subject as Windows writes it for an MSIX Publisher: the RDNs last to first, "A=v" joined by
// ", " (a multi-valued RDN by " + "), a value in double quotes (inner quotes doubled) when it holds
// , + = " < > # ; or a line end, or starts or ends with a space. False for a name it cannot write.
static bool windows_name(proven_allocator_t alloc, rp_der_span_t name, char *out, size_t cap) {
    rp_der_span_t ns = name;
    rp_der_t seq;
    if (!rp_der_get(&ns, RP_DER_SEQUENCE, &seq)) return false;
    rp_der_span_t rdns[64];
    size_t nr = 0;
    rp_der_span_t rs = rp_der_inside(&seq);
    rp_der_t set;
    while (rs.n) {
        if (nr == 64 || !rp_der_get(&rs, RP_DER_SET, &set)) return false;
        rdns[nr++] = rp_der_inside(&set);
    }
    rp_buf_t b = rp_buf_new(alloc, cap);
    bool ok = true;
    for (size_t i = nr; ok && i-- > 0;) {
        if (i + 1 < nr) rp_buf_puts(&b, ", ");
        rp_der_span_t as = rdns[i];
        bool first = true;
        rp_der_t atv;
        while (ok && as.n) {
            rp_der_t oid, val;
            ok = rp_der_get(&as, RP_DER_SEQUENCE, &atv);
            rp_der_span_t vs = ok ? rp_der_inside(&atv) : (rp_der_span_t){ 0 };
            ok = ok && rp_der_get(&vs, RP_DER_OID, &oid) && rp_der_read(&vs, &val);
            if (!ok) break;
            if (!first) rp_buf_puts(&b, " + ");
            first = false;
            const char *sn = NULL;
            for (size_t k = 0; k < sizeof short_names / sizeof short_names[0] && !sn; ++k) {
                if (rp_der_oid_is(&oid, short_names[k].oid, short_names[k].n)) sn = short_names[k].name;
            }
            if (sn) {
                rp_buf_puts(&b, sn);
            } else {                                     // OID.1.2.3
                rp_buf_puts(&b, "OID.");
                uint64_t acc = 0;
                bool head = true;
                char t[32];
                for (size_t k = 0; k < oid.val.n; ++k) {
                    acc = acc << 7 | (oid.val.p[k] & 0x7F);
                    if (oid.val.p[k] & 0x80) continue;
                    if (head) {
                        unsigned first_arc = acc < 40 ? 0 : acc < 80 ? 1 : 2;
                        snprintf(t, sizeof t, "%u.%llu", first_arc, (unsigned long long)(acc - 40 * first_arc));
                        head = false;
                    } else {
                        snprintf(t, sizeof t, ".%llu", (unsigned long long)acc);
                    }
                    rp_buf_puts(&b, t);
                    acc = 0;
                }
            }
            rp_buf_byte(&b, '=');
            rp_buf_t v = rp_buf_new(alloc, 4096);
            ok = value_text(&val, &v);
            bool quote = v.len && (v.data[0] == ' ' || v.data[v.len - 1] == ' ');
            for (size_t k = 0; k < v.len && !quote; ++k) quote = strchr(",+=\"<>#;\r\n", v.data[k]) != NULL && v.data[k];
            if (quote) rp_buf_byte(&b, '"');
            for (size_t k = 0; k < v.len; ++k) {
                if (v.data[k] == '"') rp_buf_byte(&b, '"');
                rp_buf_byte(&b, v.data[k]);
            }
            if (quote) rp_buf_byte(&b, '"');
            rp_buf_free(&v);
        }
    }
    ok = ok && b.err == PROVEN_OK && b.len < cap;
    if (ok) {
        memcpy(out, b.data, b.len);
        out[b.len] = 0;
    }
    rp_buf_free(&b);
    return ok;
}

bool rp_msix_publisher_of(proven_allocator_t alloc, const uint8_t *cert, size_t len, char *out, size_t cap) {
    rp_cert_t c;
    return rp_cert_parse(cert, len, &c, NULL) && windows_name(alloc, c.subject, out, cap);
}

// ---- signing --------------------------------------------------------------------------------------

static char why_buf[1400];

static const rp_zip_entry_t *entry(const rp_zip_entry_t *e, size_t n, const char *name) {
    for (size_t i = 0; i < n; ++i) {
        if (strcmp(e[i].name, name) == 0) return &e[i];
    }
    return NULL;
}

static proven_err_t sha256_of(proven_allocator_t alloc, const uint8_t *pkg, size_t len, const rp_zip_entry_t *e, uint8_t h[32],
                              const char **why) {
    uint8_t *plain = NULL;
    proven_err_t err = rp_zip_data(alloc, pkg, len, e, (uint64_t)1 << 30, &plain, why);
    if (err != PROVEN_OK) return err;
    rp_hash(RP_HASH_SHA256, plain, (size_t)e->size, h);
    rp_mem_free(alloc, plain);
    return PROVEN_OK;
}

static void put_record(rp_buf_t *b, const char *tag, const uint8_t h[32]) {
    rp_buf_put(b, tag, 4);
    rp_buf_put(b, h, 32);
}

// Deflates `data` as one part and adds it (the footprint files are deflated, as Windows does).
static proven_err_t add_deflated(proven_allocator_t alloc, rp_zip_writer_t *z, const char *name, const uint8_t *data, size_t n) {
    uint8_t *seg = NULL;
    size_t sl = 0;
    proven_err_t err = rp_deflate_segment(alloc, data, n, LEVEL, &seg, &sl);
    if (err != PROVEN_OK) return err;
    rp_buf_t c = rp_buf_new(alloc, sl + 8);
    rp_buf_put(&c, seg, sl);
    rp_buf_put(&c, RP_DEFLATE_END, 2);
    rp_mem_free(alloc, seg);
    size_t lfh = 0;
    if (c.err == PROVEN_OK) rp_zip_add(z, name, 8, c.data, c.len, proven_crc32((proven_mem_view_t){ (const proven_byte_t *)(data), n }), n, &lfh);
    err = c.err;
    rp_buf_free(&c);
    return err;
}

// The signature entry as Windows' signer writes it: deflated, sizes in the local header.
static proven_err_t add_deflated_plain(proven_allocator_t alloc, rp_zip_writer_t *z, const char *name, const uint8_t *data, size_t n) {
    uint8_t *seg = NULL;
    size_t sl = 0;
    proven_err_t err = rp_deflate_segment(alloc, data, n, LEVEL, &seg, &sl);
    if (err != PROVEN_OK) return err;
    rp_buf_t c = rp_buf_new(alloc, sl + 8);
    rp_buf_put(&c, seg, sl);
    rp_buf_put(&c, RP_DEFLATE_END, 2);
    rp_mem_free(alloc, seg);
    if (c.err == PROVEN_OK) rp_zip_add_plain(z, name, 8, c.data, c.len, proven_crc32((proven_mem_view_t){ (const proven_byte_t *)(data), n }), n);
    err = c.err;
    rp_buf_free(&c);
    return err;
}

// ---- AppxMetadata/CodeIntegrity.cat (RFC-0016 3) --------------------------------------------------
//
// Windows' signer adds a catalog of the package's program files, signed with the package's key:
// a SignedData whose content is a certificate trust list (1.3.6.1.4.1.311.10.1) - catalog list
// usage, a list identifier, the time, member algorithm V2, then per PE file two members (its SHA-1
// with the member-info attribute, its SHA-256 with that and an SpcIndirectData for a PE image), and
// the PackageFullName and OSAttr name-values. Worked out from Windows-signed packages
// (tests/fixtures/p9a/oracle) and Windows' own catalog hashes (format notes F9).

#define MSOID(...) { 0x2B, 0x06, 0x01, 0x04, 0x01, 0x82, 0x37, __VA_ARGS__ }
static const uint8_t O_CTL[] = MSOID(0x0A, 0x01), O_CATALOG_LIST[] = MSOID(0x0C, 0x01, 0x01), O_MEMBER_V2[] = MSOID(0x0C, 0x01, 0x03),
                     O_MEMBERINFO2[] = MSOID(0x0C, 0x02, 0x03), O_NAMEVALUE[] = MSOID(0x0C, 0x02, 0x01), O_INDIRECT[] = MSOID(0x02, 0x01, 0x04);
static const uint8_t O_SHA256[] = { 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01 };
// SpcAttributeTypeAndOptionalValue for a PE image, as Windows writes it in a catalog member.
static const uint8_t PE_IMAGE_DATA[] = { 0x30, 0x18, 0x06, 0x0A, 0x2B, 0x06, 0x01, 0x04, 0x01, 0x82, 0x37, 0x02, 0x01, 0x0F, 0x30, 0x0A,
                                         0x03, 0x02, 0x05, 0xA0, 0xA0, 0x04, 0xA2, 0x02, 0x80, 0x00 };
static const char CI_OVERRIDE[] = "<Override PartName=\"/AppxMetadata/CodeIntegrity.cat\" ContentType=\"application/vnd.ms-pkiseccat\" />";

static void memberinfo2(rp_buf_t *b, proven_allocator_t alloc) {
    rp_buf_t a = rp_buf_new(alloc, 64), v = rp_buf_new(alloc, 16);
    rp_der_put_oid(&a, O_MEMBERINFO2, sizeof O_MEMBERINFO2);
    rp_buf_byte(&v, 0x80);
    rp_buf_byte(&v, 0x00);
    rp_der_wrap(&a, RP_DER_SET, &v);
    rp_der_wrap(b, RP_DER_SEQUENCE, &a);
}

static void name_value(rp_buf_t *attrs, proven_allocator_t alloc, const char *name, const char *value) {
    rp_buf_t nv = rp_buf_new(alloc, 1024), bmp = rp_buf_new(alloc, 256), str = rp_buf_new(alloc, 1024), a = rp_buf_new(alloc, 2048), os = rp_buf_new(alloc, 2048);
    for (const char *p = name; *p; ++p) {
        rp_buf_byte(&bmp, 0);
        rp_buf_byte(&bmp, (uint8_t)*p);
    }
    rp_der_put(&nv, RP_DER_BMP, bmp.data, bmp.len);
    rp_buf_free(&bmp);
    rp_der_put_small(&nv, 0x10010001);
    for (const char *p = value; *p; ++p) rp_buf_u16le(&str, (uint8_t)*p);     // ASCII: the identity's characters
    rp_buf_u16le(&str, 0);
    rp_der_put(&nv, RP_DER_OCTET_STRING, str.data, str.len);
    rp_buf_free(&str);
    rp_der_wrap(&os, RP_DER_SEQUENCE, &nv);
    rp_der_put_oid(&a, O_NAMEVALUE, sizeof O_NAMEVALUE);
    rp_der_put(&a, RP_DER_OCTET_STRING, os.data, os.len);
    rp_buf_free(&os);
    rp_der_wrap(attrs, RP_DER_SEQUENCE, &a);
}

// "<Name>_<Version>_<Architecture>_<ResourceId>_<PublisherId>": the publisher ID is Crockford's
// base32 of the first 8 bytes of SHA-256 over the publisher in UTF-16LE, as 13 characters.
void rp_msix_publisher_id(const char *publisher, char id[14]) {
    rp_hash_t h;
    rp_hash_init(&h, RP_HASH_SHA256);
    uint8_t u[2];
    for (const unsigned char *p = (const unsigned char *)publisher; *p; ++p) {
        u[0] = *p;
        u[1] = 0;
        rp_hash_update(&h, u, 2);
    }
    uint8_t d[32];
    rp_hash_final(&h, d);
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = v << 8 | d[i];
    static const char a32[] = "0123456789abcdefghjkmnpqrstvwxyz";
    for (int i = 0; i < 13; ++i) {
        int shift = 59 - 5 * i;
        id[i] = a32[(shift >= 0 ? v >> shift : v << -shift) & 31];
    }
    id[13] = 0;
}

static bool full_name(const char *man, size_t ml, char *out, size_t cap) {
    char name[256], ver[64], arch[32] = "neutral", res[128] = "", pub[4200];
    if (!rp_xml_attr(man, ml, "Identity", "Name", name, sizeof name) || !rp_xml_attr(man, ml, "Identity", "Version", ver, sizeof ver) ||
        !rp_xml_attr(man, ml, "Identity", "Publisher", pub, sizeof pub)) {
        return false;
    }
    (void)rp_xml_attr(man, ml, "Identity", "ProcessorArchitecture", arch, sizeof arch);
    (void)rp_xml_attr(man, ml, "Identity", "ResourceId", res, sizeof res);
    rp_hash_t h;
    rp_hash_init(&h, RP_HASH_SHA256);
    uint8_t u[2];
    for (const unsigned char *p = (const unsigned char *)pub; *p; ++p) {
        u[0] = *p;              // the subject is ASCII here (rubrapack writes it from the certificate)
        u[1] = 0;
        rp_hash_update(&h, u, 2);
    }
    uint8_t d[32];
    rp_hash_final(&h, d);
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = v << 8 | d[i];
    static const char a32[] = "0123456789abcdefghjkmnpqrstvwxyz";
    char id[14];
    for (int i = 0; i < 13; ++i) {
        int shift = 59 - 5 * i;             // 65 bits: the 64 of v and a 0 after them
        id[i] = a32[(shift >= 0 ? v >> shift : v << -shift) & 31];
    }
    id[13] = 0;
    return (size_t)snprintf(out, cap, "%s_%s_%s_%s_%s", name, ver, arch, res, id) < cap;
}

static void utc_time(int64_t t, char out[32]) {
    int64_t days = t / 86400, sec = t % 86400;
    // days since 1970-01-01 to a civil date (H. Hinnant's algorithm)
    int64_t z = days + 719468, era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097, yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = yoe + era * 400, doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    int64_t dd = doy - (153 * mp + 2) / 5 + 1, mm = mp < 10 ? mp + 3 : mp - 9;
    if (mm <= 2) ++y;
    snprintf(out, 32, "%02u%02u%02u%02u%02u%02uZ", (unsigned)(y % 100) % 100, (unsigned)mm % 100, (unsigned)dd % 100, (unsigned)(sec / 3600) % 100,
             (unsigned)(sec / 60 % 60), (unsigned)(sec % 60));
}

static proven_err_t code_integrity(proven_allocator_t alloc, const uint8_t *pkg, size_t len, const rp_zip_entry_t *e, size_t ne,
                                   const rp_keyfile_t *kf, int leaf, int64_t now, uint8_t **out, size_t *out_len, const char **why) {
    *out = NULL;
    *out_len = 0;
    rp_der_span_t *subj = rp_mem_alloc(alloc, 2 * ne + 1, sizeof *subj);
    rp_buf_t *enc = rp_mem_alloc(alloc, 2 * ne + 1, sizeof *enc);
    if (subj == NULL || enc == NULL) {
        rp_mem_free(alloc, subj);
        rp_mem_free(alloc, enc);
        return PROVEN_ERR_NOMEM;
    }
    size_t ns = 0;
    proven_err_t err = PROVEN_OK;
    uint8_t *man = NULL;
    size_t ml = 0;
    rp_hash_t idh;
    rp_hash_init(&idh, RP_HASH_SHA256);
    for (size_t i = 0; err == PROVEN_OK && i < ne; ++i) {
        if (strcmp(e[i].name, "AppxManifest.xml") == 0) {
            err = rp_zip_data(alloc, pkg, len, &e[i], 1u << 24, &man, why);
            ml = man ? (size_t)e[i].size : 0;
            continue;
        }
        if (strcmp(e[i].name, "AppxBlockMap.xml") == 0 || strcmp(e[i].name, "[Content_Types].xml") == 0 || e[i].size < 64) continue;
        uint8_t *data = NULL;
        err = rp_zip_data(alloc, pkg, len, &e[i], (uint64_t)1 << 32, &data, why);
        if (err != PROVEN_OK) break;
        uint8_t h1[20], h2[32];
        const char *nwhy = NULL;
        bool pe = data[0] == 'M' && data[1] == 'Z' && rp_pe_catalog_hashes(data, (size_t)e[i].size, h1, h2, &nwhy);
        rp_mem_free(alloc, data);
        if (!pe) continue;
        rp_hash_update(&idh, h2, 32);
        // SHA-1 member: the member information only.
        rp_buf_t m1 = rp_buf_new(alloc, 256), set1 = rp_buf_new(alloc, 256);
        rp_der_put(&m1, RP_DER_OCTET_STRING, h1, 20);
        memberinfo2(&set1, alloc);
        rp_der_wrap(&m1, RP_DER_SET, &set1);
        enc[ns] = rp_buf_new(alloc, 512);
        rp_der_wrap(&enc[ns], RP_DER_SEQUENCE, &m1);
        subj[ns] = (rp_der_span_t){ enc[ns].data, enc[ns].len };
        ++ns;
        // SHA-256 member: the member information and the PE image's indirect data.
        rp_buf_t m2 = rp_buf_new(alloc, 512), mi = rp_buf_new(alloc, 64), ind = rp_buf_new(alloc, 256), idc = rp_buf_new(alloc, 256),
                 di = rp_buf_new(alloc, 128), alg = rp_buf_new(alloc, 32), ic = rp_buf_new(alloc, 256), ia = rp_buf_new(alloc, 256);
        rp_der_put(&m2, RP_DER_OCTET_STRING, h2, 32);
        memberinfo2(&mi, alloc);
        rp_buf_put(&idc, PE_IMAGE_DATA, sizeof PE_IMAGE_DATA);
        rp_der_put_oid(&alg, O_SHA256, sizeof O_SHA256);
        rp_der_put_null(&alg);
        rp_der_wrap(&di, RP_DER_SEQUENCE, &alg);
        rp_der_put(&di, RP_DER_OCTET_STRING, h2, 32);
        rp_der_wrap(&idc, RP_DER_SEQUENCE, &di);
        rp_der_wrap(&ic, RP_DER_SEQUENCE, &idc);
        rp_der_put_oid(&ia, O_INDIRECT, sizeof O_INDIRECT);
        rp_der_wrap(&ia, RP_DER_SET, &ic);
        rp_der_wrap(&ind, RP_DER_SEQUENCE, &ia);
        rp_der_span_t attrs[2] = { { mi.data, mi.len }, { ind.data, ind.len } };
        rp_der_set_of(&m2, RP_DER_SET, attrs, 2);
        rp_buf_free(&mi);
        rp_buf_free(&ind);
        enc[ns] = rp_buf_new(alloc, 1024);
        rp_der_wrap(&enc[ns], RP_DER_SEQUENCE, &m2);
        subj[ns] = (rp_der_span_t){ enc[ns].data, enc[ns].len };
        ++ns;
    }
    char fn[600], t[32];
    if (err == PROVEN_OK && ns && (man == NULL || !full_name((const char *)man, ml, fn, sizeof fn))) {
        *why = "the manifest's Identity cannot be read for the package full name";
        err = PROVEN_ERR_INVALID_FORMAT;
    }
    if (err == PROVEN_OK && ns) {
        utc_time(now, t);
        uint8_t lid[32];
        rp_hash_update(&idh, t, 13);
        rp_hash_final(&idh, lid);                   // a list identifier of its own, from the members and the time
        rp_buf_t ctl = rp_buf_new(alloc, 1u << 24), use = rp_buf_new(alloc, 64), malg = rp_buf_new(alloc, 64), ext = rp_buf_new(alloc, 4096),
                 exts = rp_buf_new(alloc, 4096);
        rp_der_put_oid(&use, O_CATALOG_LIST, sizeof O_CATALOG_LIST);
        rp_der_wrap(&ctl, RP_DER_SEQUENCE, &use);
        rp_der_put(&ctl, RP_DER_OCTET_STRING, lid, 16);
        rp_der_put(&ctl, RP_DER_UTCTIME, t, 13);
        rp_der_put_oid(&malg, O_MEMBER_V2, sizeof O_MEMBER_V2);
        rp_der_put_null(&malg);
        rp_der_wrap(&ctl, RP_DER_SEQUENCE, &malg);
        rp_der_set_of(&ctl, RP_DER_SEQUENCE, subj, ns);       // the members in DER order
        name_value(&ext, alloc, "PackageFullName", fn);
        name_value(&ext, alloc, "OSAttr", "2:6.2");
        rp_der_wrap(&exts, RP_DER_SEQUENCE, &ext);
        rp_der_wrap(&ctl, RP_DER_CTX0, &exts);
        err = ctl.err;
        if (err == PROVEN_OK) err = rp_signed_content_build(alloc, kf, leaf, RP_HASH_SHA256, O_CTL, sizeof O_CTL, ctl.data, ctl.len, out, out_len, why);
        rp_buf_free(&ctl);
    }
    for (size_t i = 0; i < ns; ++i) rp_buf_free(&enc[i]);
    rp_mem_free(alloc, enc);
    rp_mem_free(alloc, subj);
    rp_mem_free(alloc, man);
    return err;
}

static proven_err_t sign_archive(proven_allocator_t alloc, const uint8_t *pkg, size_t len, const rp_keyfile_t *kf, int leaf, int64_t now,
                                 const rp_timestamper_t *ts, bool bundle, uint8_t **out, size_t *out_len, const char **why) {
    rp_limits_t lim = rp_limits_default();
    rp_zip_entry_t *e = NULL;
    size_t ne = 0;
    proven_err_t err = rp_zip_read(alloc, pkg, len, &lim, &e, &ne, why);
    if (err != PROVEN_OK) return err;
    const rp_zip_entry_t *ct = ne ? &e[ne - 1] : NULL, *bm = entry(e, ne, "AppxBlockMap.xml");
    uint8_t *ctp = NULL;
    rp_zip_writer_t z;
    rp_zip_begin(&z, alloc, (size_t)1 << 40);
    z.signer_form = true;                   // central records and end record as Windows' signer writes them
    bool z_open = true;
    rp_buf_t newct = rp_buf_new(alloc, 1u << 20), rec = rp_buf_new(alloc, 512), tail = rp_buf_new(alloc, 1u << 24), data = rp_buf_new(alloc, 256);
    uint8_t *sig = NULL, *cat = NULL;
    size_t sl = 0, catl = 0;
    if (ct == NULL || strcmp(ct->name, "[Content_Types].xml") != 0 || bm == NULL) {
        *why = "[Content_Types].xml is not the last entry (not a package rubrapack or Windows' packaging API wrote)";
        err = PROVEN_ERR_INVALID_FORMAT;
        goto out;
    }
    // Everything before the content types, as it is.
    for (size_t i = 0; i + 1 < ne && err == PROVEN_OK; ++i) {
        if (e[i].lfh_size != 30 + strlen(e[i].name)) {
            *why = "a local header with extra fields (not a package rubrapack or Windows' packaging API wrote)";
            err = PROVEN_ERR_INVALID_FORMAT;
            break;
        }
        size_t lfh = 0;
        rp_zip_add(&z, e[i].name, e[i].method, pkg + e[i].data_off, (size_t)e[i].csize, e[i].crc, e[i].size, &lfh);
    }
    // The catalog of the package's programs (not for a bundle: its packages carry theirs).
    if (err == PROVEN_OK && !bundle) err = code_integrity(alloc, pkg, len, e, ne, kf, leaf, now, &cat, &catl, why);
    // The content types with the signature's Override (and the catalog's).
    if (err == PROVEN_OK) err = rp_zip_data(alloc, pkg, len, ct, 1u << 20, &ctp, why);
    if (err == PROVEN_OK) {
        const char *t = (const char *)ctp, *end = NULL;
        for (size_t k = (size_t)ct->size; k >= 8 && !end; --k) {
            if (memcmp(t + k - 8, "</Types>", 8) == 0) end = t + k - 8;
        }
        if (end == NULL) {
            *why = "[Content_Types].xml has no </Types>";
            err = PROVEN_ERR_INVALID_FORMAT;
        } else {
            rp_buf_put(&newct, t, (size_t)(end - t));
            rp_buf_puts(&newct, SIG_OVERRIDE);
            if (cat) rp_buf_puts(&newct, CI_OVERRIDE);
            rp_buf_put(&newct, end, (size_t)ct->size - (size_t)(end - t));
            err = newct.err;
        }
    }
    if (err == PROVEN_OK) err = add_deflated(alloc, &z, "[Content_Types].xml", newct.data, newct.len);
    if (err == PROVEN_OK && cat) err = add_deflated_plain(alloc, &z, CI_NAME, cat, catl);
    // The record of hashes.
    uint8_t h[32];
    if (err == PROVEN_OK) {
        rp_buf_puts(&rec, "APPX");
        rp_hash(RP_HASH_SHA256, z.out.data, z.out.len, h);
        put_record(&rec, "AXPC", h);
        rp_zip_tail(&z, &tail);
        rp_hash(RP_HASH_SHA256, tail.data, tail.len, h);
        put_record(&rec, "AXCD", h);
        rp_hash(RP_HASH_SHA256, newct.data, newct.len, h);
        put_record(&rec, "AXCT", h);
        err = sha256_of(alloc, pkg, len, bm, h, why);
        if (err == PROVEN_OK) put_record(&rec, "AXBM", h);
        if (err == PROVEN_OK && cat) {
            rp_hash(RP_HASH_SHA256, cat, catl, h);
            put_record(&rec, "AXCI", h);
        }
        err = err == PROVEN_OK ? (z.out.err != PROVEN_OK ? z.out.err : rec.err != PROVEN_OK ? rec.err : tail.err) : err;
    }
    if (err == PROVEN_OK) {
        sip_info(&data, bundle);
        err = rp_authenticode_build_ex(alloc, kf, leaf, RP_HASH_SHA256, data.data, data.len, rec.data, rec.len, ts, true, &sig, &sl, why);
    }
    if (err == PROVEN_OK) {
        rp_buf_t p7x = rp_buf_new(alloc, sl + 8);
        rp_buf_puts(&p7x, "PKCX");
        rp_buf_put(&p7x, sig, sl);
        err = p7x.err;
        if (err == PROVEN_OK) err = add_deflated_plain(alloc, &z, SIG_NAME, p7x.data, p7x.len);
        rp_buf_free(&p7x);
    }
    if (err == PROVEN_OK) err = z.out.err;
    if (err == PROVEN_OK) {
        err = rp_zip_finish(&z, out, out_len);
        z_open = false;
    }
out:
    if (z_open) rp_zip_abort(&z);
    rp_mem_free(alloc, sig);
    rp_mem_free(alloc, cat);
    rp_mem_free(alloc, ctp);
    rp_buf_free(&newct);
    rp_buf_free(&rec);
    rp_buf_free(&tail);
    rp_buf_free(&data);
    rp_zip_entries_free(alloc, e, ne);
    return err;
}

proven_err_t rp_msix_sign(proven_allocator_t alloc, const uint8_t *pkg, size_t len, const rp_keyfile_t *kf, int64_t now,
                          const rp_timestamper_t *ts, uint8_t **out, size_t *out_len, const char **why) {
    *out = NULL;
    *out_len = 0;
    int leaf;
    if (!rp_sign_check_key(kf, now, &leaf, why)) return PROVEN_ERR_INVALID_ARG;
    rp_limits_t lim = rp_limits_default();
    rp_msix_file_t *files = NULL;
    size_t nf = 0, ml = 0;
    uint8_t *man = NULL;
    proven_err_t err = rp_msix_open(alloc, pkg, len, &lim, &files, &nf, &man, &ml, why);
    if (err != PROVEN_OK) return err;
    bool bundle = strstr((const char *)man, "<Bundle") != NULL;
    rp_zip_entry_t *e = NULL;
    size_t ne = 0;
    err = rp_zip_read(alloc, pkg, len, &lim, &e, &ne, why);
    if (err == PROVEN_OK && (entry(e, ne, SIG_NAME) || entry(e, ne, CI_NAME))) {
        *why = "the package is signed already; rubrapack signs unsigned packages (and never twice)";
        err = PROVEN_ERR_INVALID_STATE;
    }
    rp_zip_entries_free(alloc, e, ne);
    // RFC-0011 W3: the manifest's Publisher must be the certificate's subject as Windows writes it.
    char publisher[4200], subject[4200];
    rp_cert_t lc;
    if (err == PROVEN_OK) {
        if (!rp_xml_attr((const char *)man, ml, "Identity", "Publisher", publisher, sizeof publisher)) {
            *why = "the manifest has no Identity Publisher";
            err = PROVEN_ERR_INVALID_FORMAT;
        } else if (!rp_cert_parse(kf->certs[leaf], kf->cert_len[leaf], &lc, why) || !windows_name(alloc, lc.subject, subject, sizeof subject)) {
            *why = "the certificate's subject cannot be written as an MSIX publisher";
            err = PROVEN_ERR_INVALID_ARG;
        } else if (strcmp(publisher, subject) != 0) {
            snprintf(why_buf, sizeof why_buf,
                     "the package's publisher \"%.600s\" is not the certificate's subject \"%.600s\"; set [msix] publisher to it (without --unsigned-test)",
                     publisher, subject);
            *why = why_buf;
            err = PROVEN_ERR_INVALID_ARG;
        }
    }
    // A bundle's packages first (as Windows does, RFC-0011 W4), then a new bundle around them.
    uint8_t *rebuilt = NULL;
    size_t rl = 0;
    if (err == PROVEN_OK && bundle) {
        rp_msix_part_t *parts = rp_mem_alloc(alloc, nf ? nf : 1, sizeof *parts);
        uint8_t **signed_pkgs = rp_mem_alloc(alloc, nf ? nf : 1, sizeof *signed_pkgs);
        size_t np = 0;
        if (parts == NULL || signed_pkgs == NULL) err = PROVEN_ERR_NOMEM;
        for (size_t i = 1; err == PROVEN_OK && i < nf; ++i) {      // files[0] is the bundle manifest
            size_t sl = 0;
            err = rp_msix_sign(alloc, files[i].data, (size_t)files[i].size, kf, now, ts, &signed_pkgs[np], &sl, why);
            if (err == PROVEN_OK) {
                parts[np] = (rp_msix_part_t){ files[i].name, signed_pkgs[np], sl };
                ++np;
            }
        }
        if (err == PROVEN_OK) err = rp_msix_bundle(alloc, parts, np, &lim, &rebuilt, &rl, why);
        for (size_t i = 0; signed_pkgs && i < np; ++i) rp_mem_free(alloc, signed_pkgs[i]);
        rp_mem_free(alloc, signed_pkgs);
        rp_mem_free(alloc, parts);
    }
    if (err == PROVEN_OK) {
        err = sign_archive(alloc, rebuilt ? rebuilt : pkg, rebuilt ? rl : len, kf, leaf, now, ts, bundle, out, out_len, why);
    }
    rp_mem_free(alloc, rebuilt);
    rp_msix_files_free(alloc, files, nf);
    rp_mem_free(alloc, man);
    return err;
}

// ---- checking -------------------------------------------------------------------------------------

void rp_msix_verify(proven_allocator_t alloc, const uint8_t *pkg, size_t len, rp_authenticode_check_t *r, uint8_t **sig,
                    const char **why) {
    memset(r, 0, sizeof *r);
    *sig = NULL;
    rp_limits_t lim = rp_limits_default();
    rp_msix_file_t *files = NULL;
    size_t nf = 0, ml = 0, ne = 0;
    uint8_t *man = NULL, *p7 = NULL;
    rp_zip_entry_t *e = NULL;
    rp_buf_t rec = rp_buf_new(alloc, 512), cd = rp_buf_new(alloc, 1u << 24), data = rp_buf_new(alloc, 256);
    if (rp_msix_open(alloc, pkg, len, &lim, &files, &nf, &man, &ml, why) != PROVEN_OK) goto out;
    bool bundle = strstr((const char *)man, "<Bundle") != NULL;
    if (rp_zip_read(alloc, pkg, len, &lim, &e, &ne, why) != PROVEN_OK) goto out;
    const rp_zip_entry_t *s = entry(e, ne, SIG_NAME), *ct = entry(e, ne, "[Content_Types].xml"), *bm = entry(e, ne, "AppxBlockMap.xml"),
                         *ci = entry(e, ne, CI_NAME);
    if (s == NULL) {
        *why = "the package is not signed (no AppxSignature.p7x)";
        goto out;
    }
    for (size_t i = 0; i < ne; ++i) {
        if (e[i].lfh_off > s->lfh_off) {
            *why = "AppxSignature.p7x is not the last entry";
            goto out;
        }
    }
    if (rp_zip_data(alloc, pkg, len, s, 1u << 24, &p7, why) != PROVEN_OK) goto out;
    if (s->size < 4 || memcmp(p7, "PKCX", 4) != 0) {
        *why = "AppxSignature.p7x does not start with PKCX";
        goto out;
    }
    *sig = p7;
    p7 = NULL;
    rp_authenticode_verify(*sig + 4, (size_t)s->size - 4, NULL, r, why);
    if (!r->parsed) goto out;
    // What the signature says it covers: the SIP (package or bundle) and the record of hashes.
    sip_info(&data, bundle);
    if (data.err != PROVEN_OK || r->data.n != data.len || memcmp(r->data.p, data.data, data.len) != 0) {
        *why = bundle ? "the signature is not an MSIX bundle signature" : "the signature is not an MSIX package signature";
        r->digest_ok = false;
        goto out;
    }
    uint8_t h[32];
    rp_buf_puts(&rec, "APPX");
    rp_hash(RP_HASH_SHA256, pkg, (size_t)s->lfh_off, h);
    put_record(&rec, "AXPC", h);
    if (!rp_zip_central_without(pkg, len, SIG_NAME, s->lfh_off, &cd, why)) goto out;
    rp_hash(RP_HASH_SHA256, cd.data, cd.len, h);
    put_record(&rec, "AXCD", h);
    if (ct == NULL || bm == NULL || sha256_of(alloc, pkg, len, ct, h, why) != PROVEN_OK) goto out;
    put_record(&rec, "AXCT", h);
    if (sha256_of(alloc, pkg, len, bm, h, why) != PROVEN_OK) goto out;
    put_record(&rec, "AXBM", h);
    if (ci) {
        if (sha256_of(alloc, pkg, len, ci, h, why) != PROVEN_OK) goto out;
        put_record(&rec, "AXCI", h);
    }
    r->digest_ok = rec.err == PROVEN_OK && r->signed_digest.n == rec.len && memcmp(r->signed_digest.p, rec.data, rec.len) == 0;
    if (!r->digest_ok) *why = "the package's contents do not match its signature (it was changed after signing)";
    // A bundle's packages are signed too, each on its own.
    for (size_t i = 1; bundle && r->digest_ok && i < nf; ++i) {
        rp_authenticode_check_t ir;
        uint8_t *isig = NULL;
        const char *iwhy = NULL;
        rp_msix_verify(alloc, files[i].data, (size_t)files[i].size, &ir, &isig, &iwhy);
        if (!(ir.parsed && ir.digest_ok && ir.attrs_ok && ir.signature_ok)) {
            char inner[700];
            snprintf(inner, sizeof inner, "%s", iwhy ? iwhy : "its signature does not verify");     // iwhy may be why_buf
            snprintf(why_buf, sizeof why_buf, "the package %.300s inside the bundle: %.650s", files[i].name, inner);
            *why = why_buf;
            r->digest_ok = false;
        }
        rp_mem_free(alloc, isig);
    }
out:
    rp_mem_free(alloc, p7);
    rp_buf_free(&rec);
    rp_buf_free(&cd);
    rp_buf_free(&data);
    rp_zip_entries_free(alloc, e, ne);
    rp_msix_files_free(alloc, files, nf);
    rp_mem_free(alloc, man);
}
