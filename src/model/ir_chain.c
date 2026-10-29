// src/model/ir_chain.c - chains of packages and their setup program (include/rubrapack/chain.h,
// RFC-0016 3).

#include "ir_int.h"
#include "rubrapack/cfb.h"
#include "rubrapack/lang.h"
#include "rubrapack/chain.h"
#include "rubrapack/crypto.h"
#include "rubrapack/in.h"
#include "rubrapack/msi.h"
#include "rubrapack/msix.h"
#include "rubrapack/suminfo.h"
#include "rubrapack/parts.h"
#include "rubrapack/str.h"

enum { TRAILER = 32, FLAG_VITAL = 1, FLAG_MSIX = 2, FLAG_MSP = 4, FLAG_BUNDLE = 8, CHAIN_ELEVATE = 1, MAX_PACKAGE = 1 << 30 };     // a package up to 1 GiB

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
    ir_check_format(&c);
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
    static const char *const keys[] = { "name", "manufacturer", "version", "arch", "elevate", "languages", NULL };
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
    // languages: the setup program's own texts in these languages too (English and the built-in
    // packs always); each needs its pack - lang/<code>.po beside the source, or where packs are found.
    const rp_tkey_t *lk = ir_find_key(chain, "languages");
    if (lk && lk->val.kind != RP_TV_ARRAY) ERR(&c, lk->pos, "RP1306", "languages is an array of language codes, like [\"ko\"]");
    for (size_t i = 0; lk && lk->val.kind == RP_TV_ARRAY && i < lk->val.count; ++i) {
        const rp_tval_t *v = &lk->val.items[i];
        const char *code = v->kind == RP_TV_STRING ? v->str : "";
        if (strcmp(code, "en") == 0) continue;
        if (rp_langpack_get(code, opt->source_dir ? opt->source_dir : ".") == NULL) {
            ERR(&c, lk->pos, "RP1509", "language '%s' has no language pack (lang/%s.po beside the source, or RUBRAPACK_LANG_DIR)", code, code);
        } else if (out->language_count < 8) {
            snprintf(out->languages[out->language_count++], 16, "%s", code);
        }
    }
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
            // RFC-0031 B14: an MSIX package or bundle, or a patch, beside MSI packages.
            p->kind = rp_ascii_ends_with_ci(p->shown, ".msi")  ? RP_CHAIN_MSI
                      : rp_ascii_ends_with_ci(p->shown, ".msp") ? RP_CHAIN_MSP
                      : rp_ascii_ends_with_ci(p->shown, ".msix") || rp_ascii_ends_with_ci(p->shown, ".msixbundle") ? RP_CHAIN_MSIX
                                                                                                                      : (rp_chain_kind_t)-1;
            if ((int)p->kind < 0) {
                ERR(&c, ir_key_pos(t, "source"), "RP1308", "source must be an .msi, .msix, .msixbundle or .msp package");
            } else if (p->kind == RP_CHAIN_MSIX && p->properties) {
                ERR(&c, ir_key_pos(t, "properties"), "RP1308", "properties are for Windows Installer packages; an MSIX takes none");
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
    if (rp_cfb_open(&cfb, alloc, data, len, limits, &why) != PROVEN_OK) return TR_N("is not a compound file");
    const char *bad = NULL;
    size_t t;
    rp_msi_rows_t rows = { 0 };
    if (rp_msi_open(&msi, alloc, &cfb, limits, &why) != PROVEN_OK) {
        bad = TR_N("is not a Windows Installer database");
    } else {
        if (rp_msi_find_table(&msi, "Property", &t) != PROVEN_OK || rp_msi_read_rows(&msi, t, &rows) != PROVEN_OK || rows.column_count < 2) {
            bad = TR_N("has no Property table");
        }
        for (size_t r = 0; bad == NULL && r < rows.row_count; ++r) {
            rp_msi_value_t key = rp_msi_rcell(&rows, r, 0), val = rp_msi_rcell(&rows, r, 1);
            const uint8_t *k, *v;
            size_t kn, vn;
            if (key.kind != RP_MSI_STR || val.kind != RP_MSI_STR || rp_msi_string(&msi, key.s, &k, &kn) != PROVEN_OK ||
                rp_msi_string(&msi, val.s, &v, &vn) != PROVEN_OK) {
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
        if (bad == NULL && (*code == NULL || *version == NULL)) bad = TR_N("has no ProductCode or ProductVersion");
        rp_msi_rows_free(&msi, &rows);
        rp_msi_close(&msi);
    }
    rp_cfb_close(&cfb);
    return bad;
}

// An MSIX package's or bundle's family name and Identity Version (copies), or a reason it is not one.
static const char *msix_codes(proven_allocator_t alloc, const uint8_t *data, size_t len, const rp_limits_t *limits,
                              char **code, char **version) {
    rp_msix_file_t *files = NULL;
    uint8_t *man = NULL;
    size_t nf = 0, ml = 0;
    const char *why = NULL, *bad = NULL;
    char name[256], publisher[8400], ver[64];
    *code = *version = NULL;
    if (rp_msix_open(alloc, data, len, limits, &files, &nf, &man, &ml, &why) != PROVEN_OK) {
        bad = TR_N("is not a valid MSIX package");
    } else if (!rp_xml_attr((const char *)man, ml, "Identity", "Name", name, sizeof name) ||
               !rp_xml_attr((const char *)man, ml, "Identity", "Publisher", publisher, sizeof publisher) ||
               !rp_xml_attr((const char *)man, ml, "Identity", "Version", ver, sizeof ver)) {
        bad = TR_N("has no identity (Name, Publisher and Version)");
    } else {
        char id[14];
        rp_msix_publisher_id(publisher, id);
        size_t cn = strlen(name) + 15, vn = strlen(ver) + 1;
        *code = rp_mem_alloc(alloc, cn, 1);
        *version = rp_mem_alloc(alloc, vn, 1);
        if (*code) snprintf(*code, cn, "%s_%s", name, id);
        if (*version) memcpy(*version, ver, vn);
    }
    rp_msix_files_free(alloc, files, nf);
    rp_mem_free(alloc, man);
    return bad;
}

// A patch's code (summary property 9) and the product codes it targets (7), or a reason it is not one.
static const char *msp_codes(proven_allocator_t alloc, const uint8_t *data, size_t len, const rp_limits_t *limits,
                             char **code, char **version) {
    static const uint16_t sname[] = { 5, 'S', 'u', 'm', 'm', 'a', 'r', 'y', 'I', 'n', 'f', 'o', 'r', 'm', 'a', 't', 'i', 'o', 'n' };
    rp_cfb_t cfb;
    const char *why = NULL, *bad = NULL;
    *code = *version = NULL;
    if (rp_cfb_open(&cfb, alloc, data, len, limits, &why) != PROVEN_OK) return TR_N("is not a compound file");
    uint32_t id;
    uint8_t *sum = NULL;
    size_t sn = 0;
    rp_suminfo_t si = { .count = 0 };
    if (rp_cfb_find(&cfb, 0, sname, sizeof sname / sizeof sname[0], &id) != PROVEN_OK || cfb.entries[id].size > limits->max_metadata ||
        (sum = rp_mem_alloc(alloc, (sn = (size_t)cfb.entries[id].size) + 1, 1)) == NULL || rp_cfb_read(&cfb, id, sum, sn) != PROVEN_OK ||
        rp_suminfo_parse(sum, sn, &si) != PROVEN_OK) {
        bad = TR_N("has no summary information");
    }
    const rp_suminfo_prop_t *patch = NULL, *targets = NULL, *storages = NULL;
    for (size_t i = 0; bad == NULL && i < si.count; ++i) {
        if (si.props[i].type != RP_VT_LPSTR) continue;
        if (si.props[i].pid == 9) patch = &si.props[i];
        else if (si.props[i].pid == 7) targets = &si.props[i];
        else if (si.props[i].pid == 8) storages = &si.props[i];
    }
    if (bad == NULL && (patch == NULL || patch->str_len != 38 || targets == NULL || targets->str_len == 0 || storages == NULL || storages->str_len == 0)) {
        bad = TR_N("is not a patch: it has no patch code, target products or transforms");
    }
    if (bad == NULL) {
        *code = rp_mem_alloc(alloc, patch->str_len + 1, 1);
        *version = rp_mem_alloc(alloc, targets->str_len + 1, 1);
        if (*code) memcpy(*code, patch->str, patch->str_len), (*code)[patch->str_len] = '\0';
        if (*version) memcpy(*version, targets->str, targets->str_len), (*version)[targets->str_len] = '\0';
    }
    rp_mem_free(alloc, sum);
    rp_cfb_close(&cfb);
    return bad;
}

// The setup program's texts a language pack may give (msgctxt "installer"), in the order
// src/setup/rubrapack_setup.c reads them.
static const char *const setup_texts[] = {
    "Setup.Damaged", "Setup.Ask", "Setup.Repair", "Setup.Remove", "Setup.Cancel", "Setup.RemoveAsk", "Setup.Removed",
    "Setup.RemoveFailed", "Setup.TempFailed", "Setup.RepairFailed", "Setup.InstallFailed", "Setup.Repaired", "Setup.Finished",
    "Setup.Restart",
};

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
        const char *bad = p->kind == RP_CHAIN_MSIX ? msix_codes(alloc, data, n, limits, &code, &version)
                          : p->kind == RP_CHAIN_MSP ? msp_codes(alloc, data, n, limits, &code, &version)
                                                    : msi_codes(alloc, data, n, limits, &code, &version);
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
            uint32_t kind = p->kind == RP_CHAIN_MSP ? FLAG_MSP
                            : p->kind == RP_CHAIN_MSIX ? FLAG_MSIX | (rp_ascii_ends_with_ci(p->shown, ".msixbundle") ? FLAG_BUNDLE : 0)
                                                       : 0;
            rp_buf_u32le(&m, (p->vital ? FLAG_VITAL : 0) | kind);
            rp_buf_u64le(&m, offset);
            rp_buf_u64le(&m, n);
            rp_buf_put(&m, sha, 32);
        }
        rp_mem_free(alloc, code);
        rp_mem_free(alloc, version);
        rp_mem_free(alloc, data);
    }
    // The setup program's texts in other languages than its own English: after the packages, a
    // block "RPTEXTS1", the number of languages, and for each its code, its LANGIDs and the texts
    // in the order of setup_texts (an empty one: English). The built-in packs, then [chain] languages.
    if (err == PROVEN_OK) {
        const char *codes[64];
        size_t ncodes = 0;
        for (size_t i = 0; i < rp_lang_embedded_count && ncodes < 64; ++i) codes[ncodes++] = rp_lang_embedded[i].code;
        for (size_t i = 0; i < c->language_count && ncodes < 64; ++i) {
            bool have = false;
            for (size_t j = 0; j < ncodes; ++j) have |= strcmp(codes[j], c->languages[i]) == 0;
            if (!have) codes[ncodes++] = c->languages[i];
        }
        rp_buf_put(&m, "RPTEXTS1", 8);
        rp_buf_u32le(&m, (uint32_t)ncodes);
        for (size_t i = 0; i < ncodes; ++i) {
            const rp_langpack_t *p = rp_langpack_get(codes[i], NULL);
            put_str(&m, codes[i]);
            rp_buf_u32le(&m, p ? (uint32_t)p->langid_count : 0);
            for (size_t j = 0; p && j < p->langid_count; ++j) rp_buf_u32le(&m, p->langids[j]);
            for (size_t t = 0; t < sizeof setup_texts / sizeof setup_texts[0]; ++t) {
                const char *tr = rp_langpack_text(p, "installer", setup_texts[t]);
                put_str(&m, tr ? tr : "");
            }
        }
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

proven_err_t rp_chain_read(proven_allocator_t alloc, const uint8_t *exe, size_t len, char *name, size_t name_cap,
                           rp_chain_entry_t **entries, size_t *count) {
    if (exe == NULL || entries == NULL || count == NULL) return PROVEN_ERR_INVALID_ARG;
    *entries = NULL;
    *count = 0;
    // The payload ends where a signature starts (the PE certificate table), else at the file's end.
    rp_in_t in = rp_in(exe, len);
    size_t end = len;
    if (len >= 0x40 && rp_in8(&in, 0) == 'M' && rp_in8(&in, 1) == 'Z') {
        uint32_t pe = rp_in32(&in, 0x3C);
        const uint8_t *sig = (uint64_t)pe + 24 + 112 + 40 <= len ? rp_in_bytes(&in, pe, 4) : NULL;
        if (sig != NULL && memcmp(sig, "PE\0\0", 4) == 0) {
            uint16_t magic = rp_in16(&in, (size_t)pe + 24);
            size_t dirs = pe + (magic == 0x20B ? 24 + 112 : 24 + 96);
            uint32_t cert = rp_in32(&in, dirs + 32), cert_size = rp_in32(&in, dirs + 36);
            if (cert && cert_size && cert <= len) end = cert;
        }
    }
    const uint8_t *mark = end >= TRAILER ? rp_in_bytes(&in, end - 8, 8) : NULL;
    if (mark == NULL || memcmp(mark, "RPCHAIN!", 8) != 0) return PROVEN_ERR_NOT_FOUND;
    rp_in_t t = rp_in_sub(&in, end - TRAILER, TRAILER);
    uint64_t start = rp_in64(&t, 0), mat = rp_in64(&t, 8), mlen = rp_in32(&t, 16);
    if (t.bad || start > mat || mat > end - TRAILER || mlen > end - TRAILER - mat || mlen < 16) return PROVEN_ERR_INVALID_FORMAT;
    rp_in_t mv = rp_in_sub(&in, (size_t)mat, (size_t)mlen);
    const uint8_t *magic1 = rp_in_bytes(&mv, 0, 8);
    if (magic1 == NULL || memcmp(magic1, "RPCHAIN1", 8) != 0) return PROVEN_ERR_INVALID_FORMAT;
    uint32_t n = rp_in32(&mv, 8);
    if (n == 0 || n > RP_CHAIN_MAX) return PROVEN_ERR_INVALID_FORMAT;
    rp_chain_entry_t *e = rp_mem_alloc(alloc, n + 1, sizeof *e + mlen + 16);    // entries, then the strings
    if (e == NULL) return PROVEN_ERR_NOMEM;
    char *strs = (char *)(e + n + 1), *sp = strs;
    size_t at = 16;
    const char *s[4];
#define TAKE(dst)                                                                         \
    do {                                                                                  \
        if (mlen - at < 4 || rp_in32(&mv, at) > mlen - at - 4) goto bad;                      \
        uint32_t l_ = rp_in32(&mv, at);                                                       \
        if (!rp_in_copy(&mv, at + 4, sp, l_)) goto bad;                                   \
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
        uint32_t fl = rp_in32(&mv, at);
        e[i].vital = (fl & FLAG_VITAL) != 0;
        e[i].kind = fl & FLAG_MSP ? RP_CHAIN_MSP : fl & FLAG_MSIX ? RP_CHAIN_MSIX : RP_CHAIN_MSI;
        e[i].ext = fl & FLAG_MSP ? ".msp" : fl & FLAG_BUNDLE ? ".msixbundle" : fl & FLAG_MSIX ? ".msix" : ".msi";
        uint64_t off = rp_in64(&mv, at + 4), size = rp_in64(&mv, at + 12);
        const uint8_t *want = rp_in_bytes(&mv, at + 20, 32);
        if (want == NULL || off > mat - start || size > mat - start - off) goto bad;
        e[i].data = rp_in_bytes(&in, (size_t)(start + off), (size_t)size);
        if (e[i].data == NULL) goto bad;
        e[i].size = size;
        uint8_t sha[32];
        rp_hash(RP_HASH_SHA256, e[i].data, (size_t)size, sha);
        e[i].hash_ok = memcmp(sha, want, 32) == 0;
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
