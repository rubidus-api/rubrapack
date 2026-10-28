// src/cli/winget.c - `build --winget DIR` (RFC-0031 B28; rubrapack/winget.h): the winget manifest of
// the package just written, in the multi-file form of schema 1.12.0. Every value is written as a
// double-quoted YAML scalar, which takes JSON's string syntax (rp_json_str).

#include "rubrapack/winget.h"

#include "rubrapack/diag.h"
#include "rubrapack/inspect.h"
#include "rubrapack/json.h"
#include "rubrapack/limits.h"
#include "rubrapack/mem.h"
#include "rubrapack/msix.h"
#include "rubrapack/pal.h"
#include "rubrapack/str.h"
#include "rubrapack/suminfo.h"
#include "rubrapack/version.h"
#include "rubrapack/zip.h"

#include <stdio.h>
#include <string.h>

#include "proven/hash.h"

static char *copy_text(proven_allocator_t alloc, const char *s, bool *ok) {
    if (s == NULL) return NULL;
    size_t n = strlen(s) + 1;
    char *d = rp_mem_alloc(alloc, n, 1);
    if (d == NULL) *ok = false;
    else memcpy(d, s, n);
    return d;
}

bool rp_winget_take(proven_allocator_t alloc, const rp_ir_t *ir, rp_winget_t *w) {
    memset(w, 0, sizeof *w);
    bool ok = true;
    w->id = copy_text(alloc, ir->wg_id, &ok);
    w->url = copy_text(alloc, ir->wg_url, &ok);
    w->license = copy_text(alloc, ir->wg_license, &ok);
    w->desc = copy_text(alloc, ir->wg_desc, &ok);
    w->publisher_url = copy_text(alloc, ir->wg_publisher_url, &ok);
    w->package_url = copy_text(alloc, ir->wg_package_url, &ok);
    w->license_url = copy_text(alloc, ir->wg_license_url, &ok);
    w->moniker = copy_text(alloc, ir->wg_moniker, &ok);
    w->name = copy_text(alloc, ir->has_msix && ir->msix_display ? ir->msix_display : ir->name, &ok);
    w->publisher = copy_text(alloc, ir->has_msix && ir->msix_publisher_display ? ir->msix_publisher_display : ir->manufacturer, &ok);
    w->locale = ir->language == 1042 ? "ko-KR" : "en-US";
    w->scope = ir->scope;
    if (ir->wg_tag_count) {
        w->tags = rp_mem_alloc(alloc, ir->wg_tag_count, sizeof *w->tags);
        if (w->tags == NULL) ok = false;
        for (size_t k = 0; w->tags && k < ir->wg_tag_count; ++k) w->tags[w->tag_count++] = copy_text(alloc, ir->wg_tags[k], &ok);
    }
    if (ir->wg_desc_by_lang_count) {
        w->descs = rp_mem_alloc(alloc, ir->wg_desc_by_lang_count, sizeof *w->descs);
        if (w->descs == NULL) ok = false;
        for (size_t k = 0; w->descs && k < ir->wg_desc_by_lang_count; ++k) {
            w->descs[k] = (rp_winget_text_t){ copy_text(alloc, ir->wg_desc_by_lang[k].lang, &ok), copy_text(alloc, ir->wg_desc_by_lang[k].text, &ok) };
            ++w->desc_count;
        }
    }
    return ok;
}

void rp_winget_free(proven_allocator_t alloc, rp_winget_t *w) {
    char *s[] = { w->id, w->url, w->license, w->desc, w->publisher_url, w->package_url, w->license_url, w->moniker, w->name, w->publisher };
    for (size_t k = 0; k < sizeof s / sizeof s[0]; ++k) rp_mem_free(alloc, s[k]);
    for (size_t k = 0; k < w->tag_count; ++k) rp_mem_free(alloc, w->tags[k]);
    rp_mem_free(alloc, w->tags);
    for (size_t k = 0; k < w->desc_count; ++k) {
        rp_mem_free(alloc, w->descs[k].lang);
        rp_mem_free(alloc, w->descs[k].text);
    }
    rp_mem_free(alloc, w->descs);
    memset(w, 0, sizeof *w);
}

// ---- what the package says ------------------------------------------------------------------------

enum { MAX_ARCH = 4 };

typedef struct {
    bool msix;
    char version[64];
    char product_code[64], upgrade_code[64];        // an MSI's
    char family[320], min_os[32];                   // an MSIX's
    char archs[MAX_ARCH][16];
    size_t narch;
    char sha[65], sig_sha[65];                      // the file's and its AppxSignature.p7x's (signed MSIX only)
} facts_t;

static void hex_sha(const uint8_t *data, size_t len, char out[65]) {
    proven_byte_t d[PROVEN_SHA256_SIZE];
    proven_sha256((proven_mem_view_t){ (const proven_byte_t *)data, len }, d);
    for (size_t i = 0; i < PROVEN_SHA256_SIZE; ++i) snprintf(out + 2 * i, 3, "%02X", d[i]);
}

static const char *winget_arch(const char *a) {
    if (rp_ascii_ieq(a, "x64") || rp_ascii_ieq(a, "amd64")) return "x64";
    if (rp_ascii_ieq(a, "x86") || rp_ascii_ieq(a, "intel")) return "x86";
    if (rp_ascii_ieq(a, "arm64")) return "arm64";
    if (rp_ascii_ieq(a, "neutral")) return "neutral";
    return NULL;
}

static void add_arch(facts_t *f, const char *a) {
    const char *w = winget_arch(a);
    if (w == NULL || f->narch == MAX_ARCH) return;
    for (size_t k = 0; k < f->narch; ++k) {
        if (strcmp(f->archs[k], w) == 0) return;
    }
    snprintf(f->archs[f->narch++], sizeof f->archs[0], "%s", w);
}

static const char *property(const rp_msi_wdb_t *db, const char *name, char *out, size_t cap) {
    out[0] = 0;
    for (size_t t = 0; t < db->table_count; ++t) {
        const rp_msi_wtable_t *tb = &db->tables[t];
        if (strcmp(tb->name, "Property") != 0 || tb->column_count < 2) continue;
        for (size_t r = 0; r < tb->row_count; ++r) {
            const rp_msi_cell_t *c = &tb->cells[r * tb->column_count];
            if (c[0].kind == RP_MSI_STR && c[0].len == strlen(name) && memcmp(c[0].bytes, name, c[0].len) == 0 && c[1].kind == RP_MSI_STR) {
                snprintf(out, cap, "%.*s", (int)c[1].len, (const char *)c[1].bytes);
            }
        }
    }
    return out;
}

static int msi_facts(proven_allocator_t alloc, const char *path, const rp_limits_t *lim, facts_t *f) {
    rp_pkg_t p;
    int rc = rp_pkg_open(alloc, path, lim, &p);
    if (rc == RP_EXIT_OK) {
        const rp_msi_wdb_t *db = &p.view.db;
        property(db, "ProductVersion", f->version, sizeof f->version);
        property(db, "ProductCode", f->product_code, sizeof f->product_code);
        property(db, "UpgradeCode", f->upgrade_code, sizeof f->upgrade_code);
        rp_suminfo_t si = { .count = 0 };
        if (db->summary && rp_suminfo_parse(db->summary, db->summary_len, &si) == PROVEN_OK) {
            for (size_t k = 0; k < si.count; ++k) {
                if (si.props[k].pid != 7 || si.props[k].str == NULL) continue;      // Template: "x64;1033"
                char a[16];
                size_t n = 0;
                while (n < si.props[k].str_len && n < sizeof a - 1 && si.props[k].str[n] != ';') a[n] = (char)si.props[k].str[n], ++n;
                a[n] = 0;
                add_arch(f, n ? a : "x86");
            }
        }
    }
    rp_pkg_close(alloc, &p);
    return rc;
}

// The values of every `attribute` on an element named `element` (a bundle's Packages).
static void each_attr(const char *xml, size_t len, const char *element, const char *attribute, facts_t *f) {
    char open[64];
    snprintf(open, sizeof open, "<%s ", element);
    const char *end = xml + len;
    for (const char *p = xml; p < end;) {
        const char *e = strstr(p, open);
        if (e == NULL || e >= end) break;
        const char *close = strchr(e, '>');
        if (close == NULL) break;
        char v[32];
        if (rp_xml_attr(e, (size_t)(close - e + 1), element, attribute, v, sizeof v)) add_arch(f, v);
        p = close + 1;
    }
}

static int msix_facts(proven_allocator_t alloc, const uint8_t *data, size_t len, const rp_limits_t *lim, const char *path, facts_t *f) {
    rp_msix_file_t *files = NULL;
    size_t nf = 0, ml = 0, ne = 0;
    uint8_t *man = NULL;
    const char *why = NULL;
    int rc = RP_EXIT_OK;
    char name[256] = "", publisher[8400] = "";
    if (rp_msix_open(alloc, data, len, lim, &files, &nf, &man, &ml, &why) != PROVEN_OK) {
        rp_diag_error(RP_DIAG_BAD_PACKAGE, "'%s' is not a valid MSIX package: %s", path, why ? why : "unreadable");
        rc = RP_EXIT_IO;
    } else {
        const char *m = (const char *)man;
        bool bundle = strstr(m, "<Bundle") != NULL;
        (void)rp_xml_attr(m, ml, "Identity", "Name", name, sizeof name);
        (void)rp_xml_attr(m, ml, "Identity", "Publisher", publisher, sizeof publisher);
        (void)rp_xml_attr(m, ml, "Identity", "Version", f->version, sizeof f->version);
        if (bundle) {
            each_attr(m, ml, "Package", "Architecture", f);
            (void)rp_xml_attr(m, ml, "b4:TargetDeviceFamily", "MinVersion", f->min_os, sizeof f->min_os);
        } else {
            char a[32];
            if (rp_xml_attr(m, ml, "Identity", "ProcessorArchitecture", a, sizeof a)) add_arch(f, a);
            (void)rp_xml_attr(m, ml, "TargetDeviceFamily", "MinVersion", f->min_os, sizeof f->min_os);
        }
        char id[14];
        rp_msix_publisher_id(publisher, id);
        snprintf(f->family, sizeof f->family, "%s_%s", name, id);
    }
    rp_msix_files_free(alloc, files, nf);
    rp_mem_free(alloc, man);
    // The signature, when there is one: SignatureSha256 is the hash of AppxSignature.p7x.
    rp_zip_entry_t *e = NULL;
    if (rc == RP_EXIT_OK && rp_zip_read(alloc, data, len, lim, &e, &ne, &why) == PROVEN_OK) {
        for (size_t k = 0; k < ne; ++k) {
            if (strcmp(e[k].name, "AppxSignature.p7x") != 0) continue;
            uint8_t *p7 = NULL;
            if (rp_zip_data(alloc, data, len, &e[k], 1u << 24, &p7, &why) == PROVEN_OK) hex_sha(p7, (size_t)e[k].size, f->sig_sha);
            rp_mem_free(alloc, p7);
        }
    }
    rp_zip_entries_free(alloc, e, ne);
    return rc;
}

// ---- writing ---------------------------------------------------------------------------------------

static void kv(rp_buf_t *b, const char *indent, const char *key, const char *value) {
    if (value == NULL || value[0] == 0) return;
    rp_buf_puts(b, indent);
    rp_buf_puts(b, key);
    rp_buf_puts(b, ": ");
    rp_json_str(b, value, strlen(value));
    rp_buf_byte(b, '\n');
}

static void head(rp_buf_t *b, const char *type, const rp_winget_t *w, const facts_t *f) {
    rp_buf_puts(b, "# Created by rubrapack " RUBRAPACK_VERSION_STRING "\n# yaml-language-server: $schema=https://aka.ms/winget-manifest.");
    rp_buf_puts(b, type);
    rp_buf_puts(b, ".1.12.0.schema.json\n\n");
    kv(b, "", "PackageIdentifier", w->id);
    kv(b, "", "PackageVersion", f->version);
}

static void tail(rp_buf_t *b, const char *type) {
    kv(b, "", "ManifestType", type);
    kv(b, "", "ManifestVersion", "1.12.0");
}

static bool write_one(proven_allocator_t alloc, const char *dir, const char *id, const char *suffix, rp_buf_t *b) {
    char path[2400];
    snprintf(path, sizeof path, "%s/%s%s.yaml", dir, id, suffix);
    if (b->err != PROVEN_OK || rp_pal_write_file_atomic(alloc, path, b->data, b->len) != PROVEN_OK) {
        rp_diag_error(RP_DIAG_OUTPUT, "cannot write '%s'", path);
        return false;
    }
    return true;
}

// ---- several MSIs, one manifest --------------------------------------------------------------------

// One MSI is one architecture. Its Installers entry holds all the MSI says (Scope, ProductCode,
// UpgradeCode), so the entries of several MSIs sit side by side: a build of the same identifier and
// version into a directory that holds rubrapack's installer file for it keeps that file's entries for
// the other architectures and replaces the one for its own. Entries are in a fixed order (x86, x64,
// arm64), whichever was built first. Any other file there is written over, as before.

typedef struct {
    const char *p;
    size_t      n, rank;
} entry_t;

static size_t arch_rank(const char *a, size_t n) {
    static const char *const order[] = { "x86", "x64", "arm64", "neutral" };
    for (size_t k = 0; k < sizeof order / sizeof order[0]; ++k) {
        if (strlen(order[k]) == n && memcmp(order[k], a, n) == 0) return k;
    }
    return sizeof order / sizeof order[0];
}

static const char *find(const char *s, const char *end, const char *what) {
    size_t n = strlen(what);
    for (const char *p = s; p + n <= end; ++p) {
        if (memcmp(p, what, n) == 0) return p;
    }
    return NULL;
}

static void msi_entry(rp_buf_t *b, const rp_winget_t *w, const facts_t *f) {
    kv(b, "- ", "Architecture", f->narch ? f->archs[0] : "x86");
    if (w->scope != 2) kv(b, "  ", "Scope", w->scope == 1 ? "user" : "machine");
    kv(b, "  ", "InstallerUrl", w->url);
    kv(b, "  ", "InstallerSha256", f->sha);
    kv(b, "  ", "ProductCode", f->product_code);
    if (f->upgrade_code[0]) {
        rp_buf_puts(b, "  AppsAndFeaturesEntries:\n");
        kv(b, "  - ", "UpgradeCode", f->upgrade_code);
    }
}

// The entries of the MSI just built and of the other architectures already in the directory.
static bool msi_entries(proven_allocator_t alloc, const char *dir, const rp_winget_t *w, const facts_t *f, rp_buf_t *b) {
    rp_buf_t mine = rp_buf_new(alloc, 1u << 16);
    msi_entry(&mine, w, f);
    entry_t e[8];
    size_t ne = 0;
    e[ne++] = (entry_t){ (const char *)mine.data, mine.len, arch_rank(f->narch ? f->archs[0] : "x86", f->narch ? strlen(f->archs[0]) : 3) };
    char path[2400];
    snprintf(path, sizeof path, "%s/%s.installer.yaml", dir, w->id);
    rp_buf_t vb = rp_buf_new(alloc, 256);
    rp_buf_puts(&vb, "\nPackageIdentifier: ");
    rp_json_str(&vb, w->id, strlen(w->id));
    rp_buf_puts(&vb, "\nPackageVersion: ");
    rp_json_str(&vb, f->version, strlen(f->version));
    rp_buf_puts(&vb, "\nInstallerType: \"msi\"\nInstallers:\n");
    uint8_t *old = NULL;
    size_t olen = 0;
    if (vb.err == PROVEN_OK && rp_pal_stat(alloc, path, &(uint64_t){ 0 }) == RP_FS_FILE && rp_pal_read_file(alloc, path, 1u << 20, &old, &olen) == PROVEN_OK) {
        const char *s = (const char *)old, *end = s + olen;
        rp_buf_byte(&vb, 0);
        const char *at = olen > 23 && memcmp(s, "# Created by rubrapack ", 23) == 0 ? find(s, end, (const char *)vb.data) : NULL;
        const char *from = at ? at + vb.len - 1 : NULL;
        const char *stop = from ? find(from, end, "\nManifestType: ") : NULL;
        // Each entry starts at a line "- Architecture: "; the file's other lines start in column 0.
        for (const char *p = from; stop && p <= stop && ne < sizeof e / sizeof e[0];) {
            if (end - p < 17 || memcmp(p, "- Architecture: \"", 17) != 0) break;
            const char *q = p + 1;
            while (q < stop && !(q[0] == '\n' && q[1] != ' ')) ++q;
            const char *a = p + 17, *ae = memchr(a, '"', (size_t)(q - a));
            size_t r = ae ? arch_rank(a, (size_t)(ae - a)) : 99;
            if (r != e[0].rank) e[ne++] = (entry_t){ p, (size_t)(q - p) + 1, r };
            p = q + 1;
        }
    }
    // Insertion sort by architecture.
    for (size_t i = 1; i < ne; ++i) {
        for (size_t j = i; j > 0 && e[j].rank < e[j - 1].rank; --j) {
            entry_t t = e[j];
            e[j] = e[j - 1];
            e[j - 1] = t;
        }
    }
    for (size_t i = 0; i < ne; ++i) rp_buf_put(b, e[i].p, e[i].n);
    bool ok = mine.err == PROVEN_OK && vb.err == PROVEN_OK;
    rp_mem_free(alloc, old);
    rp_buf_free(&vb);
    rp_buf_free(&mine);
    return ok;
}

int rp_winget_write(proven_allocator_t alloc, const rp_winget_t *w, const char *pkg, const char *dir) {
    rp_limits_t lim = rp_limits_default();
    uint8_t *data = NULL;
    size_t len = 0;
    if (rp_pal_read_file(alloc, pkg, (size_t)lim.max_output, &data, &len) != PROVEN_OK) {
        rp_diag_error(RP_DIAG_INPUT, "cannot read '%s'", pkg);
        return RP_EXIT_IO;
    }
    facts_t f = { 0 };
    f.msix = rp_ascii_ends_with_ci(pkg, ".msix") || rp_ascii_ends_with_ci(pkg, ".msixbundle");
    hex_sha(data, len, f.sha);
    int rc = f.msix ? msix_facts(alloc, data, len, &lim, pkg, &f) : msi_facts(alloc, pkg, &lim, &f);
    rp_mem_free(alloc, data);
    if (rc != RP_EXIT_OK) return rc;
    uint64_t size = 0;
    if (rp_pal_stat(alloc, dir, &size) == RP_FS_NONE && rp_pal_mkdir_new(alloc, dir) != PROVEN_OK) {
        rp_diag_error(RP_DIAG_OUTPUT, "cannot write '%s'", dir);
        return RP_EXIT_IO;
    }
    rp_buf_t b = rp_buf_new(alloc, 1u << 20);
    bool ok = true;
    // The version file.
    head(&b, "version", w, &f);
    kv(&b, "", "DefaultLocale", w->locale);
    tail(&b, "version");
    ok = write_one(alloc, dir, w->id, "", &b);
    // The installer file: one installer per architecture, all the same file for a bundle. An MSI's
    // entry carries all it says, so several MSIs (one per architecture) share one file.
    b.len = 0;
    head(&b, "installer", w, &f);
    kv(&b, "", "InstallerType", f.msix ? "msix" : "msi");
    if (f.msix) {
        kv(&b, "", "PackageFamilyName", f.family);
        kv(&b, "", "MinimumOSVersion", f.min_os);
    }
    rp_buf_puts(&b, "Installers:\n");
    if (f.msix) {
        for (size_t k = 0; k < f.narch; ++k) {
            kv(&b, "- ", "Architecture", f.archs[k]);
            kv(&b, "  ", "InstallerUrl", w->url);
            kv(&b, "  ", "InstallerSha256", f.sha);
            if (f.sig_sha[0]) kv(&b, "  ", "SignatureSha256", f.sig_sha);
        }
    } else {
        ok = ok && msi_entries(alloc, dir, w, &f, &b);
    }
    tail(&b, "installer");
    ok = ok && write_one(alloc, dir, w->id, ".installer", &b);
    // The default locale, then one file for each other language's short description.
    b.len = 0;
    head(&b, "defaultLocale", w, &f);
    kv(&b, "", "PackageLocale", w->locale);
    kv(&b, "", "Publisher", w->publisher);
    kv(&b, "", "PublisherUrl", w->publisher_url);
    kv(&b, "", "PackageName", w->name);
    kv(&b, "", "PackageUrl", w->package_url);
    kv(&b, "", "License", w->license);
    kv(&b, "", "LicenseUrl", w->license_url);
    kv(&b, "", "ShortDescription", w->desc);
    kv(&b, "", "Moniker", w->moniker);
    if (w->tag_count) {
        rp_buf_puts(&b, "Tags:\n");
        for (size_t k = 0; k < w->tag_count; ++k) {
            rp_buf_puts(&b, "- ");
            rp_json_str(&b, w->tags[k], strlen(w->tags[k]));
            rp_buf_byte(&b, '\n');
        }
    }
    tail(&b, "defaultLocale");
    char suffix[64];
    snprintf(suffix, sizeof suffix, ".locale.%s", w->locale);
    ok = ok && write_one(alloc, dir, w->id, suffix, &b);
    for (size_t k = 0; ok && k < w->desc_count; ++k) {
        b.len = 0;
        head(&b, "locale", w, &f);
        kv(&b, "", "PackageLocale", w->descs[k].lang);
        kv(&b, "", "ShortDescription", w->descs[k].text);
        tail(&b, "locale");
        snprintf(suffix, sizeof suffix, ".locale.%s", w->descs[k].lang);
        ok = write_one(alloc, dir, w->id, suffix, &b);
    }
    rp_buf_free(&b);
    return ok ? RP_EXIT_OK : RP_EXIT_IO;
}
