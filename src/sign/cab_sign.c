// src/sign/cab_sign.c - Authenticode for cabinets (include/rubrapack/sign.h; RFC-0031 B13).
//
// No specification describes where a cabinet keeps its signature; this is how Windows' signtool
// writes it (tests/fixtures/cab-sign, made by scripts/x96-oracle.sh; the format notes and
// manual/formats/authenticode.md tell the same):
//  - the header gets reserved space (flag 0x0004): cbCFHeader 20, cbCFFolder 0, cbCFData 0, then
//    20 bytes abReserve = 00 00 10 00, u32 offset of the signature, u32 its size, 8 zero bytes; a
//    cabinet without reserved space gets it inserted, one with header space of another size (makecab's
//    ReservePerCabinetSize) gets it resized (cbCabinet, coffFiles and every folder's coffCabStart
//    move with it); reserved space in folders or data blocks is refused;
//  - the signature follows the cabinet (at cbCabinet), zero-filled to a multiple of 8 bytes;
//  - the digest is the hash of the cabinet's first cbCabinet bytes without reserved1 (4..8),
//    cbCFHeader (36..38) and abReserve (40..60);
//  - the indirect data is 1.3.6.1.4.1.311.2.1.28 with a serialized object (class
//    A6B586D5-B4A1-2466-AE05-A217DA8E60D6) whose one attribute 1.3.6.1.4.1.311.2.5.1 holds the
//    SHA-256 of cbCabinet as 4 little-endian bytes;
//  - the signed attributes are signtool's: an empty SpcSpOpusInfo.

#include "rubrapack/lang.h"
#include "rubrapack/mem.h"
#include "rubrapack/sign.h"

#include <string.h>

enum { HEADER = 36, RESERVE_PRESENT = 4, PREV_CABINET = 1, NEXT_CABINET = 2, RESERVE = 20, INSERTED = 4 + RESERVE };

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

// The header's layout: whether it has signtool's reserved space, and where the folders start.
typedef struct {
    uint32_t cb;            // cbCabinet
    size_t   reserve;       // bytes of reserved header space with its sizes (0, or 4 + cbCFHeader)
    bool     reserved;      // signtool's: cbCFHeader 20, cbCFFolder 0, cbCFData 0, abReserve 00 00 10 00
    uint32_t sig_off, sig_size;
    size_t   folders;       // offset of the first CFFOLDER
    uint16_t nfolders;
} cab_t;

static bool cab_layout(const uint8_t *c, size_t len, cab_t *l, const char **why) {
    memset(l, 0, sizeof *l);
    if (len < HEADER || memcmp(c, "MSCF", 4) != 0) {
        *why = TR_N("not a cabinet (no MSCF header)");
        return false;
    }
    l->cb = rd32(c + 8);
    l->nfolders = rd16(c + 26);
    uint16_t flags = rd16(c + 30);
    size_t at = HEADER;
    if (l->cb < HEADER || l->cb > len) {
        *why = TR_N("a damaged cabinet header");
        return false;
    }
    if (flags & RESERVE_PRESENT) {
        if (l->cb < HEADER + 4 || c[38] != 0 || c[39] != 0 || (size_t)rd16(c + 36) + HEADER + 4 > l->cb) {
            *why = TR_N("the cabinet has reserved space in its folders or data blocks, which rubrapack does not lay out");
            return false;
        }
        l->reserve = 4 + (size_t)rd16(c + 36);
        l->reserved = rd16(c + 36) == RESERVE && rd32(c + 40) == 0x00100000u;
        if (l->reserved) {
            l->sig_off = rd32(c + 44);
            l->sig_size = rd32(c + 48);
        }
        at += l->reserve;
    }
    for (int k = 0; k < 2; ++k) {                       // szCabinetPrev/szDiskPrev, szCabinetNext/szDiskNext
        if (!(flags & (k == 0 ? PREV_CABINET : NEXT_CABINET))) continue;
        for (int s = 0; s < 2; ++s) {
            while (at < l->cb && c[at]) ++at;
            if (at++ >= l->cb) {
                *why = TR_N("a damaged cabinet header");
                return false;
            }
        }
    }
    l->folders = at;
    if (l->folders + (size_t)l->nfolders * 8 > l->cb) {
        *why = TR_N("a damaged cabinet header");
        return false;
    }
    return true;
}

// The digest of a cabinet with the reserved space.
static void cab_digest(const uint8_t *c, uint32_t cb, uint8_t *digest) {
    rp_hash_t h;
    rp_hash_init(&h, RP_HASH_SHA256);
    rp_hash_update(&h, c, 4);
    rp_hash_update(&h, c + 8, HEADER - 8);
    rp_hash_update(&h, c + 38, 2);
    rp_hash_update(&h, c + 40 + RESERVE, cb - (40 + RESERVE));
    rp_hash_final(&h, digest);
}

// SpcAttributeTypeAndOptionalValue for a cabinet of cbCabinet `cb`, as signtool writes it.
static size_t cab_data(uint32_t cb, uint8_t out[160]) {
    static const uint8_t head[] = {
        0x30, 0x56, 0x06, 0x0A, 0x2B, 0x06, 0x01, 0x04, 0x01, 0x82, 0x37, 0x02, 0x01, 0x1C,      // 1.3.6.1.4.1.311.2.1.28
        0xA1, 0x48, 0x04, 0x10, 0xA6, 0xB5, 0x86, 0xD5, 0xB4, 0xA1, 0x24, 0x66, 0xAE, 0x05, 0xA2, 0x17, 0xDA, 0x8E, 0x60, 0xD6,
        0x04, 0x34, 0x31, 0x32, 0x30, 0x30, 0x06, 0x0A, 0x2B, 0x06, 0x01, 0x04, 0x01, 0x82, 0x37, 0x02, 0x05, 0x01,  // 1.3.6.1.4.1.311.2.5.1
        0x31, 0x22, 0x04, 0x20,
    };
    uint8_t le[4];
    wr32(le, cb);
    memcpy(out, head, sizeof head);
    rp_hash(RP_HASH_SHA256, le, 4, out + sizeof head);
    return sizeof head + 32;
}

proven_err_t rp_cab_prepare(proven_allocator_t alloc, const uint8_t *cab, size_t len, uint8_t **out, size_t *out_len, const char **why) {
    cab_t l;
    if (!cab_layout(cab, len, &l, why)) return PROVEN_ERR_INVALID_FORMAT;
    if (l.reserved && (l.sig_off || l.sig_size)) {
        *why = TR_N("the file is signed already; rubrapack does not sign twice or replace a signature");
        return PROVEN_ERR_INVALID_STATE;
    }
    // The cabinet with signtool's reserved space in place of what it had: everything after the
    // header's reserved space moves by `delta`.
    int64_t delta = (int64_t)INSERTED - (int64_t)l.reserve;
    uint64_t n = (uint64_t)((int64_t)l.cb + delta);
    if (n + 65536 > 0xFFFFFFF0u) {
        *why = TR_N("the file is too large to sign");
        return PROVEN_ERR_OUT_OF_BOUNDS;
    }
    uint8_t *o = rp_mem_alloc(alloc, (size_t)n, 1);
    if (o == NULL) return PROVEN_ERR_NOMEM;
    memcpy(o, cab, HEADER);
    memset(o + HEADER, 0, INSERTED);
    o[36] = RESERVE;
    o[42] = 0x10;               // abReserve begins 00 00 10 00
    memcpy(o + HEADER + INSERTED, cab + HEADER + l.reserve, l.cb - HEADER - l.reserve);
    o[30] |= RESERVE_PRESENT;
    wr32(o + 8, (uint32_t)n);
    wr32(o + 16, (uint32_t)((int64_t)rd32(o + 16) + delta));       // coffFiles
    for (size_t f = 0; f < l.nfolders; ++f) {                         // coffCabStart
        uint8_t *p = o + (size_t)((int64_t)l.folders + delta) + f * 8;
        wr32(p, (uint32_t)((int64_t)rd32(p) + delta));
    }
    *out = o;
    *out_len = (size_t)n;
    return PROVEN_OK;
}

proven_err_t rp_cab_digest(const uint8_t *cab, size_t len, uint8_t digest[32], const char **why) {
    cab_t l;
    if (!cab_layout(cab, len, &l, why)) return PROVEN_ERR_INVALID_FORMAT;
    if (!l.reserved) {
        *why = TR_N("the cabinet has no room for a signature");
        return PROVEN_ERR_INVALID_FORMAT;
    }
    cab_digest(cab, l.cb, digest);
    return PROVEN_OK;
}

proven_err_t rp_cab_sign(proven_allocator_t alloc, const uint8_t *cab, size_t len, const rp_keyfile_t *kf, int64_t now,
                         const rp_timestamper_t *ts, uint8_t **out, size_t *out_len, const char **why) {
    int leaf;
    if (!rp_sign_check_key(kf, now, &leaf, why)) return PROVEN_ERR_INVALID_ARG;
    uint8_t *work = NULL;
    size_t wl = 0;
    proven_err_t err = rp_cab_prepare(alloc, cab, len, &work, &wl, why);
    if (err != PROVEN_OK) return err;
    uint8_t digest[32], data[160];
    cab_digest(work, (uint32_t)wl, digest);
    size_t dl = cab_data((uint32_t)wl, data);
    uint8_t *p7 = NULL;
    size_t p7_len = 0;
    err = rp_authenticode_build_ex(alloc, kf, leaf, RP_HASH_SHA256, data, dl, digest, 0, ts, RP_ATTRS_SIGNTOOL, &p7, &p7_len, why);
    size_t padded = (p7_len + 7) & ~(size_t)7;
    uint8_t *o = err == PROVEN_OK ? rp_mem_alloc(alloc, wl + padded, 1) : NULL;
    if (err == PROVEN_OK && o == NULL) err = PROVEN_ERR_NOMEM;
    if (err == PROVEN_OK) {
        memcpy(o, work, wl);
        memcpy(o + wl, p7, p7_len);
        memset(o + wl + p7_len, 0, padded - p7_len);
        wr32(o + 44, (uint32_t)wl);
        wr32(o + 48, (uint32_t)padded);
        *out = o;
        *out_len = wl + padded;
    }
    rp_mem_free(alloc, work);
    rp_mem_free(alloc, p7);
    return err;
}

void rp_cab_verify(const uint8_t *cab, size_t len, rp_authenticode_check_t *r, const char **why) {
    memset(r, 0, sizeof *r);
    cab_t l;
    if (!cab_layout(cab, len, &l, why)) return;
    if (!l.reserved || l.sig_size == 0) {
        *why = TR_N("the file is not signed");
        return;
    }
    if (l.sig_off != l.cb || l.sig_size > len - l.cb) {
        *why = TR_N("the signature does not lie where the cabinet says");
        return;
    }
    rp_der_span_t s = { cab + l.sig_off, l.sig_size };
    rp_der_t ci;
    if (!rp_der_read(&s, &ci)) {
        *why = TR_N("the signature is not DER");
        return;
    }
    uint8_t digest[32];
    cab_digest(cab, l.cb, digest);
    rp_authenticode_verify(ci.whole.p, ci.whole.n, digest, r, why);
    if (r->parsed && r->alg != RP_HASH_SHA256) {
        r->digest_ok = false;
        *why = TR_N("the cabinet's signature uses a hash rubrapack does not check (only SHA-256)");
    } else if (r->parsed && !r->digest_ok) {
        *why = TR_N("the file was changed after it was signed (its digest does not match)");
    }
}
