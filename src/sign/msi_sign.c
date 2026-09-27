// src/sign/msi_sign.c - Authenticode for MSI packages (include/rubrapack/sign.h; RFC-0007 5.4).
//
// Microsoft does not publish the MSI digest. It was worked out byte for byte against Windows' own
// signer (scripts/p6-oracle.sh; docs/research/2026-09-27-p6-authenticode.md): packages signed on
// the VM, then candidate layouts compared with the digests Windows signed, including packages whose
// directory entries were given non-zero state bits, times and CLSIDs to tell the fields apart.

#include "rubrapack/mem.h"
#include "rubrapack/msi.h"
#include "rubrapack/sign.h"

#include <string.h>

static const uint16_t SIG_NAME[] = { 5, 'D', 'i', 'g', 'i', 't', 'a', 'l', 'S', 'i', 'g', 'n', 'a', 't', 'u', 'r', 'e' };
static const uint16_t EX_NAME[] = { 5, 'M', 's', 'i', 'D', 'i', 'g', 'i', 't', 'a', 'l', 'S', 'i', 'g', 'n', 'a', 't', 'u', 'r', 'e', 'E', 'x' };

static bool is_name(const rp_cfb_entry_t *e, const uint16_t *n, size_t len) {
    return e->name_len == len && memcmp(e->name, n, len * sizeof *n) == 0;
}

// UTF-16LE byte order of two names.
static int cmp_le(const rp_cfb_entry_t *a, const rp_cfb_entry_t *b) {
    size_t n = a->name_len < b->name_len ? a->name_len : b->name_len;
    for (size_t i = 0; i < n; ++i) {
        uint8_t x0 = (uint8_t)a->name[i], y0 = (uint8_t)b->name[i];
        if (x0 != y0) return x0 < y0 ? -1 : 1;
        uint8_t x1 = (uint8_t)(a->name[i] >> 8), y1 = (uint8_t)(b->name[i] >> 8);
        if (x1 != y1) return x1 < y1 ? -1 : 1;
    }
    return a->name_len < b->name_len ? -1 : a->name_len > b->name_len ? 1 : 0;
}

static void put_le(rp_hash_t *h, const uint16_t *name, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        uint8_t b[2] = { (uint8_t)name[i], (uint8_t)(name[i] >> 8) };
        rp_hash_update(h, b, 2);
    }
}

bool rp_msi_digest(const rp_cfb_t *cfb, rp_hash_alg_t alg, bool with_ex, uint8_t ex[32], uint8_t *digest, const char **why) {
    uint32_t ids[4096];
    size_t n = 0;
    if (rp_cfb_children(cfb, 0, NULL, 0, &n) != PROVEN_OK || n > 4096 || rp_cfb_children(cfb, 0, ids, 4096, &n) != PROVEN_OK) {
        *why = "the package's directory cannot be read";
        return false;
    }
    uint32_t streams[4096];
    size_t ns = 0;
    for (size_t i = 0; i < n; ++i) {
        const rp_cfb_entry_t *e = &cfb->entries[ids[i]];
        if (e->type == RP_CFB_STORAGE) {
            *why = "the package holds storages (embedded transforms or sub-databases), which rubrapack does not sign";
            return false;
        }
        if (e->type != RP_CFB_STREAM || is_name(e, SIG_NAME, 17) || is_name(e, EX_NAME, 22)) continue;
        streams[ns++] = ids[i];
    }
    for (size_t i = 1; i < ns; ++i) {
        for (size_t j = i; j > 0 && cmp_le(&cfb->entries[streams[j - 1]], &cfb->entries[streams[j]]) > 0; --j) {
            uint32_t t = streams[j];
            streams[j] = streams[j - 1];
            streams[j - 1] = t;
        }
    }
    const rp_cfb_entry_t *root = &cfb->entries[0];
    rp_hash_t h;
    if (with_ex) {
        rp_hash_init(&h, RP_HASH_SHA256);
        rp_hash_update(&h, root->clsid, 16);
        uint8_t st[4] = { (uint8_t)root->state, (uint8_t)(root->state >> 8), (uint8_t)(root->state >> 16), (uint8_t)(root->state >> 24) };
        rp_hash_update(&h, st, 4);
        for (size_t i = 0; i < ns; ++i) {
            const rp_cfb_entry_t *e = &cfb->entries[streams[i]];
            uint8_t size[8];
            for (int k = 0; k < 8; ++k) size[k] = (uint8_t)(e->size >> (8 * k));
            put_le(&h, e->name, e->name_len);
            rp_hash_update(&h, size, 8);
            rp_hash_update(&h, e->times, 16);
        }
        rp_hash_final(&h, ex);
    }
    rp_hash_init(&h, alg);
    if (with_ex) rp_hash_update(&h, ex, 32);
    uint8_t *buf = NULL;
    size_t cap = 0;
    bool ok = true;
    for (size_t i = 0; ok && i < ns; ++i) {
        const rp_cfb_entry_t *e = &cfb->entries[streams[i]];
        if (e->size > cap) {
            rp_mem_free(cfb->alloc, buf);
            cap = (size_t)e->size;
            buf = rp_mem_alloc(cfb->alloc, cap + 1, 1);
            if (buf == NULL) ok = false;
        }
        if (ok && rp_cfb_read(cfb, streams[i], buf, cap) != PROVEN_OK) ok = false;
        if (ok) rp_hash_update(&h, buf, (size_t)e->size);
    }
    rp_mem_free(cfb->alloc, buf);
    if (!ok) {
        *why = "a stream of the package cannot be read";
        return false;
    }
    rp_hash_update(&h, root->clsid, 16);
    rp_hash_final(&h, digest);
    return true;
}

// SpcAttributeTypeAndOptionalValue for MSI, as Windows writes it: SPC_SIPINFO with version 2, the
// MSI subject interface package GUID {000C10F1-0000-0000-C000-000000000046}, and five zeros.
static const uint8_t MSI_DATA[] = { 0x30, 0x32, 0x06, 0x0A, 0x2B, 0x06, 0x01, 0x04, 0x01, 0x82, 0x37, 0x02, 0x01, 0x1E,
                                    0x30, 0x24, 0x02, 0x01, 0x02, 0x04, 0x10, 0xF1, 0x10, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x00,
                                    0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46, 0x02, 0x01, 0x00, 0x02, 0x01, 0x00, 0x02,
                                    0x01, 0x00, 0x02, 0x01, 0x00, 0x02, 0x01, 0x00 };

// Whether the Media table names a cabinet outside the package (not "#stream").
static bool has_external_cabs(proven_allocator_t alloc, const rp_cfb_t *cfb) {
    rp_limits_t lim = rp_limits_default();
    rp_msi_t msi;
    const char *why;
    if (rp_msi_open(&msi, alloc, cfb, &lim, &why) != PROVEN_OK) return false;
    rp_msi_view_t view;
    bool ext = false;
    if (rp_msi_view(&msi, NULL, 0, &view) == PROVEN_OK) {
        for (size_t t = 0; t < view.db.table_count; ++t) {
            const rp_msi_wtable_t *tb = &view.db.tables[t];
            if (strcmp(tb->name, "Media") != 0) continue;
            for (size_t c = 0; c < tb->column_count; ++c) {
                if (strcmp(tb->columns[c].name, "Cabinet") != 0) continue;
                for (size_t r = 0; r < tb->row_count; ++r) {
                    const rp_msi_cell_t *v = &tb->cells[r * tb->column_count + c];
                    if (v->kind == RP_MSI_STR && v->len && v->bytes[0] != '#') ext = true;
                }
            }
        }
        rp_msi_view_free(&msi, &view);
    }
    rp_msi_close(&msi);
    return ext;
}

// The root's streams as writer input (names and contents; the contents are owned by the caller).
static proven_err_t collect(proven_allocator_t alloc, const rp_cfb_t *cfb, rp_cfb_stream_t **out, size_t *count, uint8_t ***bufs) {
    uint32_t ids[4096];
    size_t n = 0;
    if (rp_cfb_children(cfb, 0, ids, 4096, &n) != PROVEN_OK) return PROVEN_ERR_INVALID_FORMAT;
    rp_cfb_stream_t *s = rp_mem_alloc(alloc, n + 3, sizeof *s);
    uint8_t **b = rp_mem_alloc(alloc, n + 3, sizeof *b);
    if (s == NULL || b == NULL) {
        rp_mem_free(alloc, s);
        rp_mem_free(alloc, b);
        return PROVEN_ERR_NOMEM;
    }
    size_t k = 0;
    for (size_t i = 0; i < n; ++i) {
        const rp_cfb_entry_t *e = &cfb->entries[ids[i]];
        if (e->type != RP_CFB_STREAM) continue;
        // One block: the contents, then a copy of the name (the directory goes away with the file).
        size_t name_off = ((size_t)e->size + 1 + 1) & ~(size_t)1;
        b[k] = rp_mem_alloc(alloc, name_off + 2 * (size_t)e->name_len + 2, 1);
        if (b[k] == NULL || rp_cfb_read(cfb, ids[i], b[k], (size_t)e->size) != PROVEN_OK) {
            for (size_t j = 0; j <= k; ++j) rp_mem_free(alloc, b[j]);
            rp_mem_free(alloc, s);
            rp_mem_free(alloc, b);
            return PROVEN_ERR_NOMEM;
        }
        uint16_t *name = (uint16_t *)(void *)(b[k] + name_off);
        memcpy(name, e->name, 2 * (size_t)e->name_len);
        s[k] = (rp_cfb_stream_t){ name, e->name_len, b[k], (size_t)e->size };
        ++k;
    }
    *out = s;
    *count = k;
    *bufs = b;
    return PROVEN_OK;
}

proven_err_t rp_msi_sign(proven_allocator_t alloc, const uint8_t *msi, size_t len, const rp_keyfile_t *kf, int64_t now,
                         bool allow_external_cabs, bool *external_cabs, uint8_t **out, size_t *out_len, const char **why) {
    rp_limits_t lim = rp_limits_default();
    rp_cfb_t cfb;
    *external_cabs = false;
    *why = NULL;
    if (rp_cfb_open(&cfb, alloc, msi, len, &lim, why) != PROVEN_OK) return PROVEN_ERR_INVALID_FORMAT;
    uint32_t id;
    proven_err_t err = PROVEN_OK;
    int leaf = -1;
    if (rp_cfb_find(&cfb, 0, SIG_NAME, 17, &id) == PROVEN_OK) {
        *why = "the package is signed already; rubrapack does not sign twice or replace a signature";
        err = PROVEN_ERR_INVALID_STATE;
    } else if ((*external_cabs = has_external_cabs(alloc, &cfb)) && !allow_external_cabs) {
        *why = "the package uses cabinets outside itself, which a signature of the .msi does not cover; build it with embedded "
               "cabinets, or sign anyway with --allow-unsigned-cabs (RFC-0007 S2)";
        err = PROVEN_ERR_INVALID_ARG;
    } else if (!rp_sign_check_key(kf, now, &leaf, why)) {
        err = PROVEN_ERR_INVALID_ARG;
    }
    rp_cfb_stream_t *streams = NULL;
    size_t ns = 0;
    uint8_t **bufs = NULL;
    uint8_t clsid[16];
    memcpy(clsid, cfb.entries[0].clsid, 16);
    unsigned shift = cfb.major == 4 ? 12 : 9;
    if (err == PROVEN_OK) {
        uint8_t dummy[32];
        if (!rp_msi_digest(&cfb, RP_HASH_SHA256, false, dummy, dummy, why)) err = PROVEN_ERR_INVALID_FORMAT;     // refuses storages
    }
    if (err == PROVEN_OK) err = collect(alloc, &cfb, &streams, &ns, &bufs);
    rp_cfb_close(&cfb);
    if (err != PROVEN_OK) return err;
    // The digests are taken from the file as rubrapack writes it (times and state bits of the
    // streams zero), so write it once without the signature, then once more with it.
    uint8_t *plain = NULL, *p7 = NULL;
    size_t plain_len = 0, p7_len = 0;
    uint8_t ex[32], digest[RP_HASH_MAX];
    err = rp_cfb_write(alloc, shift, clsid, streams, ns, &lim, &plain, &plain_len);
    if (err == PROVEN_OK) {
        rp_cfb_t w;
        err = rp_cfb_open(&w, alloc, plain, plain_len, &lim, why);
        if (err == PROVEN_OK) {
            if (!rp_msi_digest(&w, RP_HASH_SHA256, true, ex, digest, why)) err = PROVEN_ERR_INVALID_FORMAT;
            rp_cfb_close(&w);
        }
    }
    if (err == PROVEN_OK) err = rp_authenticode_build(alloc, kf, leaf, RP_HASH_SHA256, MSI_DATA, sizeof MSI_DATA, digest, &p7, &p7_len, why);
    if (err == PROVEN_OK) {
        streams[ns++] = (rp_cfb_stream_t){ SIG_NAME, 17, p7, p7_len };
        streams[ns++] = (rp_cfb_stream_t){ EX_NAME, 22, ex, 32 };
        err = rp_cfb_write(alloc, shift, clsid, streams, ns, &lim, out, out_len);
        ns -= 2;
    }
    if (err != PROVEN_OK && *why == NULL) *why = err == PROVEN_ERR_NOMEM ? "out of memory" : "the signed package could not be written";
    for (size_t i = 0; i < ns; ++i) rp_mem_free(alloc, bufs[i]);
    rp_mem_free(alloc, bufs);
    rp_mem_free(alloc, streams);
    rp_mem_free(alloc, plain);
    rp_mem_free(alloc, p7);
    return err;
}

void rp_msi_verify(proven_allocator_t alloc, const uint8_t *msi, size_t len, rp_authenticode_check_t *r, uint8_t **sig_out,
                   const char **why) {
    memset(r, 0, sizeof *r);
    *sig_out = NULL;
    rp_limits_t lim = rp_limits_default();
    rp_cfb_t cfb;
    if (rp_cfb_open(&cfb, alloc, msi, len, &lim, why) != PROVEN_OK) return;
    uint32_t sid, xid;
    if (rp_cfb_find(&cfb, 0, SIG_NAME, 17, &sid) != PROVEN_OK) {
        *why = "the package is not signed";
        rp_cfb_close(&cfb);
        return;
    }
    bool has_ex = rp_cfb_find(&cfb, 0, EX_NAME, 22, &xid) == PROVEN_OK;
    size_t sl = (size_t)cfb.entries[sid].size;
    uint8_t *sig = rp_mem_alloc(alloc, sl + 1, 1), stored_ex[32] = { 0 };
    bool ok = sig && rp_cfb_read(&cfb, sid, sig, sl) == PROVEN_OK;
    if (ok && has_ex) ok = cfb.entries[xid].size == 32 && rp_cfb_read(&cfb, xid, stored_ex, 32) == PROVEN_OK;
    if (!ok) {
        *why = "the signature streams cannot be read";
    } else {
        rp_authenticode_verify(sig, sl, NULL, r, why);
        uint8_t ex[32], digest[RP_HASH_MAX];
        if (r->parsed && rp_msi_digest(&cfb, r->alg, has_ex, ex, digest, why)) {
            bool ex_ok = !has_ex || memcmp(ex, stored_ex, 32) == 0;
            const char *vwhy = NULL;
            rp_authenticode_verify(sig, sl, digest, r, &vwhy);
            if (!ex_ok) r->digest_ok = false;
            *why = !r->digest_ok ? "the package was changed after it was signed (its digest does not match)" : vwhy;
        }
    }
    *sig_out = sig;
    rp_cfb_close(&cfb);
}
