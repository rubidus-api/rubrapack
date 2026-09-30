// src/model/ir_chain.c - chains of packages and their setup program (include/rubrapack/chain.h,
// RFC-0016 3).

#include "ir_int.h"
#include "rubrapack/cfb.h"
#include "rubrapack/chain.h"
#include "rubrapack/crypto.h"
#include "rubrapack/msi.h"
#include "rubrapack/parts.h"

enum { TRAILER = 32, FLAG_VITAL = 1, CHAIN_ELEVATE = 1, MAX_PACKAGE = 1 << 30 };     // a package up to 1 GiB

bool rp_chain_is(const rp_tdoc_t *doc) {
    for (size_t k = 0; doc && k < doc->count; ++k) {
        if (strcmp(doc->tables[k].kind, "chain") == 0) return true;
    }
    return false;
}

void rp_chain_free(rp_chain_t *c) {
    if (c == NULL) return;
    for (size_t i = 0; i < c->count; ++i) {
        rp_mem_free(c->alloc, c->pkgs[i].id);
        rp_mem_free(c->alloc, c->pkgs[i].source);
        rp_mem_free(c->alloc, c->pkgs[i].shown);
        rp_mem_free(c->alloc, c->pkgs[i].properties);
    }
    rp_mem_free(c->alloc, c->pkgs);
    rp_mem_free(c->alloc, c->name);
    rp_mem_free(c->alloc, c->manufacturer);
    rp_mem_free(c->alloc, c->version);
    memset(c, 0, sizeof *c);
}

proven_err_t rp_chain_parse(proven_allocator_t alloc, const rp_tdoc_t *doc, const rp_ir_options_t *opt, rp_chain_t *out,
                            rp_srcdiags_t *diags) {
    if (doc == NULL || opt == NULL || out == NULL || diags == NULL) return PROVEN_ERR_INVALID_ARG;
    memset(out, 0, sizeof *out);
    out->alloc = alloc;
    rp_ir_t dummy = { .alloc = alloc };
    ctx_t c = { .alloc = alloc, .doc = doc, .opt = opt, .d = diags, .ir = &dummy };
    size_t errors = diags->errors, npkg = 0;
    const rp_ttable_t *chain = NULL;
    for (size_t k = 0; k < doc->count; ++k) {
        const rp_ttable_t *t = &doc->tables[k];
        if (strcmp(t->kind, "chain") == 0 || strcmp(t->kind, "define") == 0) {
            if (t->id) ERR(&c, t->pos, "RP1201", "[%s] takes no ID: write [%s]", t->kind, t->kind);
            else if (strcmp(t->kind, "chain") == 0) chain = t;
            else c.define = t;
        } else if (strcmp(t->kind, "chain-package") == 0) {
            if (t->id == NULL) ERR(&c, t->pos, "RP1201", "[chain-package] needs an ID: write [chain-package.ID]");
            else ++npkg;
        } else {
            ERR(&c, t->pos, "RP1201", "a chain holds [chain], [chain-package.ID] and [define] only, not [%s]; the packages are built from their own sources",
                t->kind);
        }
    }
    if (chain == NULL) {
        rp_pos_t top = { 1, 1 };
        ERR(&c, top, "RP1202", "a chain needs [chain]");
        return PROVEN_ERR_INVALID_FORMAT;
    }
    static const char *const keys[] = { "name", "manufacturer", "version", "arch", "elevate", NULL };
    ir_check_keys(&c, chain, keys);
    out->name = ir_get_str(&c, chain, "name", true, NULL);
    out->manufacturer = ir_get_str(&c, chain, "manufacturer", true, NULL);
    out->version = ir_get_str(&c, chain, "version", true, NULL);
    uint16_t vparts[4];
    size_t vcount;
    if (out->version && !ir_parse_version(out->version, vparts, &vcount)) {
        ERR(&c, ir_key_pos(chain, "version"), "RP1308", "version '%s' must be a.b.c or a.b.c.d", out->version);
    }
    char *arch = ir_get_str(&c, chain, "arch", false, NULL);
    out->arch = RP_ARCH_X64;
    if (opt->arch) {
        rp_mem_free(alloc, arch);
        arch = ir_dup(&c, opt->arch);
    }
    if (arch) {
        if (strcmp(arch, "x64") == 0) out->arch = RP_ARCH_X64;
        else if (strcmp(arch, "x86") == 0) out->arch = RP_ARCH_X86;
        else if (strcmp(arch, "arm64") == 0) out->arch = RP_ARCH_ARM64;
        else ERR(&c, ir_key_pos(chain, "arch"), "RP1308", "arch must be \"x64\", \"x86\" or \"arm64\" (the setup program's own)");
        rp_mem_free(alloc, arch);
    }
    out->elevate = ir_get_bool(&c, chain, "elevate", true);
    if (npkg == 0) ERR(&c, chain->pos, "RP1202", "a chain needs at least one [chain-package.ID]");
    if (npkg > RP_CHAIN_MAX) ERR(&c, chain->pos, "RP1308", "a chain holds at most %d packages", RP_CHAIN_MAX);
    out->pkgs = rp_mem_alloc(alloc, npkg + 1, sizeof *out->pkgs);
    if (out->pkgs == NULL) return PROVEN_ERR_NOMEM;
    memset(out->pkgs, 0, (npkg + 1) * sizeof *out->pkgs);
    static const char *const pkg_keys[] = { "source", "properties", "vital", NULL };
    for (size_t k = 0; k < doc->count && out->count < npkg; ++k) {
        const rp_ttable_t *t = &doc->tables[k];
        if (strcmp(t->kind, "chain-package") != 0 || t->id == NULL) continue;
        rp_chain_pkg_t *p = &out->pkgs[out->count++];
        ir_check_keys(&c, t, pkg_keys);
        ir_check_id(&c, t, 60);
        for (size_t j = 0; j + 1 < out->count; ++j) {
            if (ir_ascii_casecmp(out->pkgs[j].id, t->id) == 0) ERR(&c, t->pos, "RP1301", "[chain-package.%s] is there twice", t->id);
        }
        p->id = ir_dup(&c, t->id);
        p->pos = t->pos;
        p->shown = ir_get_str(&c, t, "source", true, NULL);
        p->properties = ir_get_str(&c, t, "properties", false, NULL);
        p->vital = ir_get_bool(&c, t, "vital", true);
        if (p->shown) {
            size_t n = strlen(p->shown);
            if (n < 4 || ir_ascii_casecmp(p->shown + n - 4, ".msi") != 0) {
                ERR(&c, ir_key_pos(t, "source"), "RP1308", "source must be an .msi package");
            } else if (ir_source_path_ok(&c, p->shown, ir_key_pos(t, "source"))) {
                p->source = ir_join(&c, opt->source_dir ? opt->source_dir : ".", p->shown);
                uint64_t size;
                if (p->source && rp_pal_stat(alloc, p->source, &size) != RP_FS_FILE) {
                    ERR(&c, ir_key_pos(t, "source"), "RP1507", "package '%s' not found", p->shown);
                }
            }
        }
    }
    if (c.nomem) return PROVEN_ERR_NOMEM;
    if (diags->errors != errors) {
        rp_chain_free(out);
        return PROVEN_ERR_INVALID_FORMAT;
    }
    return PROVEN_OK;
}

// ---- the setup program -----------------------------------------------------------------------

static void put_str(rp_buf_t *b, const char *s) {
    size_t n = s ? strlen(s) : 0;
    rp_buf_u32le(b, (uint32_t)n);
    if (n) rp_buf_put(b, s, n);
}

// ProductCode and ProductVersion of an MSI in memory (copies), or a reason it is not one.
static const char *msi_codes(proven_allocator_t alloc, const uint8_t *data, size_t len, const rp_limits_t *limits,
                             char **code, char **version) {
    rp_cfb_t cfb;
    rp_msi_t msi;
    const char *why = NULL;
    *code = *version = NULL;
    if (rp_cfb_open(&cfb, alloc, data, len, limits, &why) != PROVEN_OK) return "is not a compound file";
    const char *bad = NULL;
    size_t t;
    rp_msi_rows_t rows = { 0 };
    if (rp_msi_open(&msi, alloc, &cfb, limits, &why) != PROVEN_OK) {
        bad = "is not a Windows Installer database";
    } else {
        if (rp_msi_find_table(&msi, "Property", &t) != PROVEN_OK || rp_msi_read_rows(&msi, t, &rows) != PROVEN_OK || rows.column_count < 2) {
            bad = "has no Property table";
        }
        for (size_t r = 0; bad == NULL && r < rows.row_count; ++r) {
            const rp_msi_value_t *row = &rows.cells[r * rows.column_count];
            const uint8_t *k, *v;
            size_t kn, vn;
            if (row[0].kind != RP_MSI_STR || row[1].kind != RP_MSI_STR || rp_msi_string(&msi, row[0].s, &k, &kn) != PROVEN_OK ||
                rp_msi_string(&msi, row[1].s, &v, &vn) != PROVEN_OK) {
                continue;
            }
            char **dst = kn == 11 && memcmp(k, "ProductCode", 11) == 0 ? code : kn == 14 && memcmp(k, "ProductVersion", 14) == 0 ? version : NULL;
            if (dst && *dst == NULL) {
                *dst = rp_mem_alloc(alloc, vn + 1, 1);
                if (*dst) {
                    memcpy(*dst, v, vn);
                    (*dst)[vn] = '\0';
                }
            }
        }
        if (bad == NULL && (*code == NULL || *version == NULL)) bad = "has no ProductCode or ProductVersion";
        rp_msi_rows_free(&msi, &rows);
        rp_msi_close(&msi);
    }
    rp_cfb_close(&cfb);
    return bad;
}

proven_err_t rp_chain_write(proven_allocator_t alloc, const rp_chain_t *c, const rp_limits_t *limits, uint8_t **out, size_t *len,
                            rp_srcdiags_t *diags) {
    if (c == NULL || limits == NULL || out == NULL || len == NULL || diags == NULL) return PROVEN_ERR_INVALID_ARG;
    const unsigned char *stub = c->arch == RP_ARCH_X64 ? rp_setup_x64 : c->arch == RP_ARCH_X86 ? rp_setup_x86 : rp_setup_arm64;
    size_t stub_len = c->arch == RP_ARCH_X64 ? rp_setup_x64_len : c->arch == RP_ARCH_X86 ? rp_setup_x86_len : rp_setup_arm64_len;
    if (stub_len == 0) {
        rp_srcdiag_add(diags, (rp_pos_t){ 1, 1 }, "RP1901", false, "this rubrapack was built without the setup program (nob parts)");
        return PROVEN_ERR_INVALID_FORMAT;
    }
    rp_buf_t b = rp_buf_new(alloc, (size_t)limits->max_output);
    rp_buf_t m = rp_buf_new(alloc, limits->max_metadata);
    rp_buf_put(&b, stub, stub_len);
    while (b.len % 8) rp_buf_byte(&b, 0);
    uint64_t start = b.len;
    rp_buf_put(&m, "RPCHAIN1", 8);
    rp_buf_u32le(&m, (uint32_t)c->count);
    rp_buf_u32le(&m, c->elevate ? CHAIN_ELEVATE : 0);
    put_str(&m, c->name);
    put_str(&m, c->manufacturer);
    put_str(&m, c->version);
    proven_err_t err = PROVEN_OK;
    for (size_t i = 0; err == PROVEN_OK && i < c->count; ++i) {
        const rp_chain_pkg_t *p = &c->pkgs[i];
        uint8_t *data = NULL;
        size_t n = 0;
        err = rp_pal_read_file(alloc, p->source, MAX_PACKAGE, &data, &n);
        if (err != PROVEN_OK) {
            rp_srcdiag_add(diags, p->pos, "RP1507", false, "cannot read package '%s'", p->shown);
            break;
        }
        char *code = NULL, *version = NULL;
        const char *bad = msi_codes(alloc, data, n, limits, &code, &version);
        if (bad) {
            rp_srcdiag_add(diags, p->pos, "RP1308", false, "package '%s' %s", p->shown, bad);
            err = PROVEN_ERR_INVALID_FORMAT;
        } else {
            uint8_t sha[32];
            rp_hash(RP_HASH_SHA256, data, n, sha);
            uint64_t offset = b.len - start;
            rp_buf_put(&b, data, n);
            put_str(&m, p->id);
            put_str(&m, p->properties);
            put_str(&m, code);
            put_str(&m, version);
            rp_buf_u32le(&m, p->vital ? FLAG_VITAL : 0);
            rp_buf_u64le(&m, offset);
            rp_buf_u64le(&m, n);
            rp_buf_put(&m, sha, 32);
        }
        rp_mem_free(alloc, code);
        rp_mem_free(alloc, version);
        rp_mem_free(alloc, data);
    }
    uint64_t mat = b.len;
    if (err == PROVEN_OK && m.err == PROVEN_OK) rp_buf_put(&b, m.data, m.len);
    while ((b.len + TRAILER) % 8) rp_buf_byte(&b, 0);
    rp_buf_u64le(&b, start);
    rp_buf_u64le(&b, mat);
    rp_buf_u32le(&b, (uint32_t)m.len);
    rp_buf_u32le(&b, 0);
    rp_buf_put(&b, "RPCHAIN!", 8);
    if (err == PROVEN_OK) err = m.err != PROVEN_OK ? m.err : b.err;
    rp_buf_free(&m);
    if (err != PROVEN_OK) {
        rp_buf_free(&b);
        return err;
    }
    return rp_buf_take(&b, out, len);
}

// ---- reading one -----------------------------------------------------------------------------

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64(const uint8_t *p) { return rd32(p) | (uint64_t)rd32(p + 4) << 32; }

proven_err_t rp_chain_read(proven_allocator_t alloc, const uint8_t *exe, size_t len, char *name, size_t name_cap,
                           rp_chain_entry_t **entries, size_t *count) {
    if (exe == NULL || entries == NULL || count == NULL) return PROVEN_ERR_INVALID_ARG;
    *entries = NULL;
    *count = 0;
    // The payload ends where a signature starts (the PE certificate table), else at the file's end.
    size_t end = len;
    if (len >= 0x40 && exe[0] == 'M' && exe[1] == 'Z') {
        uint32_t pe = rd32(exe + 0x3C);
        if ((uint64_t)pe + 24 + 112 + 40 <= len && memcmp(exe + pe, "PE\0\0", 4) == 0) {
            uint16_t magic = (uint16_t)(exe[pe + 24] | exe[pe + 25] << 8);
            size_t dirs = pe + (magic == 0x20B ? 24 + 112 : 24 + 96);
            uint32_t cert = rd32(exe + dirs + 32), cert_size = rd32(exe + dirs + 36);
            if (cert && cert_size && cert <= len) end = cert;
        }
    }
    if (end < TRAILER || memcmp(exe + end - 8, "RPCHAIN!", 8) != 0) return PROVEN_ERR_NOT_FOUND;
    const uint8_t *t = exe + end - TRAILER;
    uint64_t start = rd64(t), mat = rd64(t + 8), mlen = rd32(t + 16);
    if (start > mat || mat > end - TRAILER || mlen > end - TRAILER - mat || mlen < 16) return PROVEN_ERR_INVALID_FORMAT;
    const uint8_t *m = exe + mat;
    if (memcmp(m, "RPCHAIN1", 8) != 0) return PROVEN_ERR_INVALID_FORMAT;
    uint32_t n = rd32(m + 8);
    if (n == 0 || n > RP_CHAIN_MAX) return PROVEN_ERR_INVALID_FORMAT;
    rp_chain_entry_t *e = rp_mem_alloc(alloc, n + 1, sizeof *e + mlen + 16);    // entries, then the strings
    if (e == NULL) return PROVEN_ERR_NOMEM;
    char *strs = (char *)(e + n + 1), *sp = strs;
    size_t at = 16;
    const char *s[4];
#define TAKE(dst)                                                                         \
    do {                                                                                  \
        if (mlen - at < 4 || rd32(m + at) > mlen - at - 4) goto bad;                      \
        uint32_t l_ = rd32(m + at);                                                       \
        memcpy(sp, m + at + 4, l_);                                                       \
        sp[l_] = '\0';                                                                    \
        (dst) = sp;                                                                       \
        sp += l_ + 1;                                                                     \
        at += 4 + (size_t)l_;                                                             \
    } while (0)
    TAKE(s[0]);
    if (name && name_cap) snprintf(name, name_cap, "%s", s[0]);
    TAKE(s[1]);
    TAKE(s[2]);
    for (uint32_t i = 0; i < n; ++i) {
        TAKE(e[i].id);
        TAKE(e[i].properties);
        TAKE(e[i].product_code);
        TAKE(e[i].version);
        if (mlen - at < 52) goto bad;
        e[i].vital = (rd32(m + at) & FLAG_VITAL) != 0;
        uint64_t off = rd64(m + at + 4), size = rd64(m + at + 12);
        if (off > mat - start || size > mat - start - off) goto bad;
        e[i].data = exe + start + off;
        e[i].size = size;
        uint8_t sha[32];
        rp_hash(RP_HASH_SHA256, e[i].data, (size_t)size, sha);
        e[i].hash_ok = memcmp(sha, m + at + 20, 32) == 0;
        at += 52;
    }
#undef TAKE
    *entries = e;
    *count = n;
    return PROVEN_OK;
bad:
    rp_mem_free(alloc, e);
    return PROVEN_ERR_INVALID_FORMAT;
}
