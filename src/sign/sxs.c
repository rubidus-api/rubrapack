// src/sign/sxs.c - the catalog of a shared Win32 assembly (include/rubrapack/sign.h; RFC-0031 B9).
//
// Windows installs a Win32 assembly into the side-by-side store (WinSxS) only with a catalog that
// covers its manifest, signed by the key whose public key token the manifest's assemblyIdentity
// names (Microsoft Learn, "Assembly Signing Example": mt -makecdfs, makecat, signtool sign; the
// certificate need not be trusted, at least 2048 bits). This writes what makecat and signtool
// write for that .cdf (tests/fixtures/sxs, made by scripts/x101-oracle.sh):
//  - a certificate trust list: catalog list usage, a 16-byte list identifier, the time, member
//    algorithm V1 (1.3.6.1.4.1.311.12.1.2), and the manifest twice - under the upper-case hex of
//    its SHA-1 and under its file name (both UTF-16LE with the terminator) - each with an
//    SpcIndirectData of a flat file (1.3.6.1.4.1.311.2.1.25, an empty link) holding that SHA-1, and
//    the member information {DE351A42-8E59-11D0-8C47-00C04FC295EE}, 512;
//  - signed with SHA-256 and signtool's signed attributes.
// The token is the last 8 bytes, reversed, of the SHA-1 of the strong-name public key blob: a
// 12-byte header (CALG_RSA_SIGN, CALG_SHA1, the blob's size) and the RSA PUBLICKEYBLOB.

#include "rubrapack/lang.h"
#include "rubrapack/mem.h"
#include "rubrapack/pki.h"
#include "rubrapack/sign.h"
#include "rubrapack/text.h"

#include <stdio.h>
#include <string.h>

#define MSOID(...) { 0x2B, 0x06, 0x01, 0x04, 0x01, 0x82, 0x37, __VA_ARGS__ }
static const uint8_t O_CTL[] = MSOID(0x0A, 0x01), O_CATALOG_LIST[] = MSOID(0x0C, 0x01, 0x01), O_MEMBER_V1[] = MSOID(0x0C, 0x01, 0x02),
                     O_MEMBERINFO[] = MSOID(0x0C, 0x02, 0x02), O_INDIRECT[] = MSOID(0x02, 0x01, 0x04), O_FLAT[] = MSOID(0x02, 0x01, 0x19);
static const uint8_t O_SHA1[] = { 0x2B, 0x0E, 0x03, 0x02, 0x1A };
static const char MEMBER_GUID[] = "{DE351A42-8E59-11D0-8C47-00C04FC295EE}";

bool rp_sxs_token(const uint8_t *cert, size_t len, char token[17], const char **why) {
    rp_cert_t c;
    if (!rp_cert_parse(cert, len, &c, why)) return false;
    if (!c.rsa || c.rsa_e.n > 4 || c.rsa_n.n < 256 || c.rsa_n.n > 2048) {
        *why = TR_N("a shared Win32 assembly's catalog needs an RSA key of 2048 bits or more");
        return false;
    }
    uint32_t e = 0;
    for (size_t i = 0; i < c.rsa_e.n; ++i) e = e << 8 | c.rsa_e.p[i];
    uint32_t bits = (uint32_t)c.rsa_n.n * 8, blob = 20 + (uint32_t)c.rsa_n.n;
    uint8_t head[32] = { 0x00, 0x24, 0, 0, 0x04, 0x80, 0, 0, (uint8_t)blob, (uint8_t)(blob >> 8), (uint8_t)(blob >> 16), (uint8_t)(blob >> 24),
                         0x06, 0x02, 0, 0, 0x00, 0x24, 0, 0, 'R', 'S', 'A', '1',
                         (uint8_t)bits, (uint8_t)(bits >> 8), (uint8_t)(bits >> 16), (uint8_t)(bits >> 24),
                         (uint8_t)e, (uint8_t)(e >> 8), (uint8_t)(e >> 16), (uint8_t)(e >> 24) };
    rp_sha1_t h;
    rp_sha1_init(&h);
    rp_sha1_update(&h, head, sizeof head);
    for (size_t i = c.rsa_n.n; i-- > 0;) rp_sha1_update(&h, &c.rsa_n.p[i], 1);     // little-endian
    uint8_t d[20];
    rp_sha1_final(&h, d);
    for (int i = 0; i < 8; ++i) snprintf(token + 2 * i, 3, "%02x", d[19 - i]);
    return true;
}

// One member: its tag (UTF-16LE with the terminator), the flat-file indirect data and the member
// information.
static void member(rp_buf_t *list, proven_allocator_t alloc, const char *tag, const uint8_t sha1[20]) {
    rp_buf_t m = rp_buf_new(alloc, 1024), u = rp_buf_new(alloc, 1024), ind = rp_buf_new(alloc, 256), mi = rp_buf_new(alloc, 256);
    proven_u16 w[600];
    rp_text_result_t n = rp_utf8_to_utf16((const uint8_t *)tag, strlen(tag), w, 600);
    for (size_t i = 0; n.err == PROVEN_OK && i < n.units; ++i) rp_buf_u16le(&u, w[i]);
    rp_buf_u16le(&u, 0);
    rp_der_put(&m, RP_DER_OCTET_STRING, u.data, u.len);
    rp_buf_free(&u);
    // SEQUENCE { 2.1.4, SET { SEQUENCE { SEQUENCE { 2.1.25, [2] { [0] } }, SEQUENCE { SEQUENCE { sha1, NULL }, OCTET STRING } } } }
    rp_buf_t a = rp_buf_new(alloc, 256), link = rp_buf_new(alloc, 64), di = rp_buf_new(alloc, 64), alg = rp_buf_new(alloc, 32),
             body = rp_buf_new(alloc, 256), set = rp_buf_new(alloc, 256);
    static const uint8_t empty_link[] = { 0xA2, 0x02, 0x80, 0x00 };
    rp_der_put_oid(&link, O_FLAT, sizeof O_FLAT);
    rp_buf_put(&link, empty_link, sizeof empty_link);
    rp_der_wrap(&body, RP_DER_SEQUENCE, &link);
    rp_der_put_oid(&alg, O_SHA1, sizeof O_SHA1);
    rp_der_put_null(&alg);
    rp_der_wrap(&di, RP_DER_SEQUENCE, &alg);
    rp_der_put(&di, RP_DER_OCTET_STRING, sha1, 20);
    rp_der_wrap(&body, RP_DER_SEQUENCE, &di);
    rp_der_wrap(&set, RP_DER_SEQUENCE, &body);
    rp_der_put_oid(&a, O_INDIRECT, sizeof O_INDIRECT);
    rp_der_wrap(&a, RP_DER_SET, &set);
    rp_der_wrap(&ind, RP_DER_SEQUENCE, &a);
    // SEQUENCE { 12.2.2, SET { SEQUENCE { BMPString guid, INTEGER 512 } } }
    rp_buf_t b = rp_buf_new(alloc, 128), bmp = rp_buf_new(alloc, 128), v = rp_buf_new(alloc, 128), vs = rp_buf_new(alloc, 128);
    for (const char *p = MEMBER_GUID; *p; ++p) {
        rp_buf_byte(&bmp, 0);
        rp_buf_byte(&bmp, (uint8_t)*p);
    }
    rp_der_put(&v, RP_DER_BMP, bmp.data, bmp.len);
    rp_buf_free(&bmp);
    rp_der_put_small(&v, 512);
    rp_der_wrap(&vs, RP_DER_SEQUENCE, &v);
    rp_der_put_oid(&b, O_MEMBERINFO, sizeof O_MEMBERINFO);
    rp_der_wrap(&b, RP_DER_SET, &vs);
    rp_der_wrap(&mi, RP_DER_SEQUENCE, &b);
    rp_buf_t attrs = rp_buf_new(alloc, 512);
    rp_buf_put(&attrs, ind.data, ind.len);
    rp_buf_put(&attrs, mi.data, mi.len);
    rp_buf_free(&ind);
    rp_buf_free(&mi);
    rp_der_wrap(&m, RP_DER_SET, &attrs);
    rp_der_wrap(list, RP_DER_SEQUENCE, &m);
}

static void utc_time(int64_t t, char out[16]) {
    int64_t days = t / 86400, sec = t % 86400;
    int64_t z = days + 719468, era = (z >= 0 ? z : z - 146096) / 146097;      // H. Hinnant's civil date
    int64_t doe = z - era * 146097, yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = yoe + era * 400, doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    int64_t dd = doy - (153 * mp + 2) / 5 + 1, mm = mp < 10 ? mp + 3 : mp - 9;
    if (mm <= 2) ++y;
    snprintf(out, 16, "%02u%02u%02u%02u%02u%02uZ", (unsigned)(y % 100) % 100, (unsigned)mm % 100, (unsigned)dd % 100, (unsigned)(sec / 3600) % 100,
             (unsigned)(sec / 60 % 60), (unsigned)(sec % 60));
}

proven_err_t rp_sxs_catalog(proven_allocator_t alloc, const char *manifest_name, const uint8_t *manifest, size_t mlen, int64_t this_update,
                            const rp_keyfile_t *kf, int64_t now, const rp_timestamper_t *ts, uint8_t **out, size_t *out_len, const char **why) {
    *out = NULL;
    *out_len = 0;
    int leaf;
    if (!rp_sign_check_key(kf, now, &leaf, why)) return PROVEN_ERR_INVALID_ARG;
    uint8_t sha1[20], id[32];
    rp_sha1_t s1;
    rp_sha1_init(&s1);
    rp_sha1_update(&s1, manifest, mlen);
    rp_sha1_final(&s1, sha1);
    char hex[41], t[16];
    for (int i = 0; i < 20; ++i) snprintf(hex + 2 * i, 3, "%02X", sha1[i]);
    utc_time(this_update, t);
    rp_hash_t h;                    // the list identifier: from the manifest and the time (makecat's is random)
    rp_hash_init(&h, RP_HASH_SHA256);
    rp_hash_update(&h, sha1, 20);
    rp_hash_update(&h, t, 13);
    rp_hash_final(&h, id);
    rp_buf_t ctl = rp_buf_new(alloc, 1u << 16), use = rp_buf_new(alloc, 64), malg = rp_buf_new(alloc, 64), members = rp_buf_new(alloc, 4096);
    rp_der_put_oid(&use, O_CATALOG_LIST, sizeof O_CATALOG_LIST);
    rp_der_wrap(&ctl, RP_DER_SEQUENCE, &use);
    rp_der_put(&ctl, RP_DER_OCTET_STRING, id, 16);
    rp_der_put(&ctl, RP_DER_UTCTIME, t, 13);
    rp_der_put_oid(&malg, O_MEMBER_V1, sizeof O_MEMBER_V1);
    rp_der_put_null(&malg);
    rp_der_wrap(&ctl, RP_DER_SEQUENCE, &malg);
    member(&members, alloc, hex, sha1);             // makecat's order: the <HASH> entry, then the name
    member(&members, alloc, manifest_name, sha1);
    rp_der_wrap(&ctl, RP_DER_SEQUENCE, &members);
    proven_err_t err = ctl.err;
    if (err == PROVEN_OK) {
        err = rp_signed_content_build_ex(alloc, kf, leaf, RP_HASH_SHA256, O_CTL, sizeof O_CTL, ctl.data, ctl.len, ts, RP_ATTRS_SIGNTOOL, out,
                                         out_len, why);
    }
    rp_buf_free(&ctl);
    return err;
}
