// src/msi/lower.c - IR -> MSI tables, cabinet and summary (include/rubrapack/build.h).
//
// Identity rules: DECISIONS 2026-09-26 "P2 identity rules". Column types are the ones msi.dll
// writes for the standard tables (P1a fixtures). One component per file, key path = the file.

#include "rubrapack/buf.h"
#include "rubrapack/build.h"
#include "rubrapack/cab.h"
#include "rubrapack/ident.h"
#include "rubrapack/md5.h"
#include "rubrapack/mem.h"
#include "rubrapack/msi.h"
#include "rubrapack/pal.h"
#include "rubrapack/suminfo.h"
#include "rubrapack/version.h"

#include <stdio.h>
#include <string.h>

#include "proven/hash.h"

// ---- string keeping --------------------------------------------------------------------------

typedef struct {
    proven_allocator_t alloc;
    char             **v;
    size_t             count, cap;
    bool               nomem;
} keep_t;

static char *keep(keep_t *k, char *s) {
    if (s == NULL) {
        k->nomem = true;
        return NULL;
    }
    if (k->count == k->cap) {
        size_t ncap = k->cap ? k->cap * 2 : 64;
        char **v = rp_mem_alloc(k->alloc, ncap, sizeof *v);
        if (v == NULL) {
            k->nomem = true;
            rp_mem_free(k->alloc, s);
            return NULL;
        }
        if (k->count) memcpy(v, k->v, k->count * sizeof *v);
        rp_mem_free(k->alloc, k->v);
        k->v = v;
        k->cap = ncap;
    }
    k->v[k->count++] = s;
    return s;
}

static char *kprintf(keep_t *k, const char *fmt, const char *a, const char *b) {
    size_t n = strlen(fmt) + (a ? strlen(a) : 0) + (b ? strlen(b) : 0) + 1;
    char *s = rp_mem_alloc(k->alloc, n, 1);
    if (s) snprintf(s, n, fmt, a ? a : "", b ? b : "");
    return keep(k, s);
}

static char *kdup(keep_t *k, const char *s) { return kprintf(k, "%s", s, NULL); }

static void keep_free(keep_t *k) {
    for (size_t i = 0; i < k->count; ++i) rp_mem_free(k->alloc, k->v[i]);
    rp_mem_free(k->alloc, k->v);
}

// ---- table rows ------------------------------------------------------------------------------

typedef struct {
    rp_msi_wtable_t  t;
    rp_msi_cell_t   *cells;
    size_t           cap;           // cells allocated
    size_t           filled;        // cells written
    proven_allocator_t alloc;
    bool             nomem;
} rows_t;

static void rows_init(rows_t *r, proven_allocator_t alloc, const char *name, const rp_msi_wcolumn_t *cols, size_t n) {
    memset(r, 0, sizeof *r);
    r->alloc = alloc;
    r->t.name = name;
    r->t.columns = cols;
    r->t.column_count = n;
}

static rp_msi_cell_t *next_cell(rows_t *r) {
    if (r->filled == r->cap) {
        size_t ncap = r->cap ? r->cap * 2 : 64;
        rp_msi_cell_t *v = rp_mem_alloc(r->alloc, ncap, sizeof *v);
        if (v == NULL) {
            r->nomem = true;
            return NULL;
        }
        if (r->filled) memcpy(v, r->cells, r->filled * sizeof *v);
        rp_mem_free(r->alloc, r->cells);
        r->cells = v;
        r->cap = ncap;
    }
    rp_msi_cell_t *c = &r->cells[r->filled++];
    memset(c, 0, sizeof *c);
    return c;
}

static void s_(rows_t *r, const char *s) {       // string cell (NULL -> null)
    rp_msi_cell_t *c = next_cell(r);
    if (c == NULL) return;
    if (s == NULL) return;
    c->kind = RP_MSI_STR;
    c->bytes = (const uint8_t *)s;
    c->len = strlen(s);
}

static void i_(rows_t *r, int32_t v) {
    rp_msi_cell_t *c = next_cell(r);
    if (c == NULL) return;
    c->kind = RP_MSI_INT;
    c->i = v;
}

static void null_(rows_t *r) { (void)next_cell(r); }

static void rows_finish(rows_t *r) {
    r->t.cells = r->cells;
    r->t.row_count = r->t.column_count ? r->filled / r->t.column_count : 0;
}

// ---- schemas (column types as msi.dll writes them) -------------------------------------------

#define KEY_S(n)  (0x2D00u | (n))
#define S(n)      (0x0D00u | (n))
#define S_N(n)    (0x1D00u | (n))
#define L(n)      (0x0F00u | (n))
#define L_N(n)    (0x1F00u | (n))
#define I2        0x0502u
#define I2_N      0x1502u
#define KEY_I2    0x2502u
#define I4        0x0104u
#define KEY_I4    0x2104u
#define KEY_S_N(n) (0x3D00u | (n))

static const rp_msi_wcolumn_t property_cols[] = { { "Property", KEY_S(72) }, { "Value", L(0) } };
static const rp_msi_wcolumn_t directory_cols[] = { { "Directory", KEY_S(72) }, { "Directory_Parent", S_N(72) },
                                                   { "DefaultDir", L(255) } };
static const rp_msi_wcolumn_t component_cols[] = { { "Component", KEY_S(72) }, { "ComponentId", S_N(38) },
                                                   { "Directory_", S(72) }, { "Attributes", I2 },
                                                   { "Condition", S_N(255) }, { "KeyPath", S_N(72) } };
static const rp_msi_wcolumn_t feature_cols[] = { { "Feature", KEY_S(38) }, { "Feature_Parent", S_N(38) },
                                                 { "Title", L_N(64) }, { "Description", L_N(255) },
                                                 { "Display", I2_N }, { "Level", I2 },
                                                 { "Directory_", S_N(72) }, { "Attributes", I2 } };
static const rp_msi_wcolumn_t featurecomp_cols[] = { { "Feature_", KEY_S(38) }, { "Component_", KEY_S(72) } };
static const rp_msi_wcolumn_t file_cols[] = { { "File", KEY_S(72) }, { "Component_", S(72) }, { "FileName", L(255) },
                                              { "FileSize", I4 }, { "Version", S_N(72) }, { "Language", S_N(20) },
                                              { "Attributes", I2_N }, { "Sequence", I4 } };
static const rp_msi_wcolumn_t filehash_cols[] = { { "File_", KEY_S(72) }, { "Options", I2 }, { "HashPart1", I4 },
                                                  { "HashPart2", I4 }, { "HashPart3", I4 }, { "HashPart4", I4 } };
static const rp_msi_wcolumn_t media_cols[] = { { "DiskId", KEY_I2 }, { "LastSequence", I4 }, { "DiskPrompt", L_N(64) },
                                               { "Cabinet", S_N(255) }, { "VolumeLabel", S_N(32) }, { "Source", S_N(72) } };
static const rp_msi_wcolumn_t upgrade_cols[] = { { "UpgradeCode", KEY_S(38) }, { "VersionMin", KEY_S_N(20) },
                                                 { "VersionMax", KEY_S_N(20) }, { "Language", KEY_S_N(255) },
                                                 { "Attributes", KEY_I4 }, { "Remove", S_N(255) },
                                                 { "ActionProperty", S(72) } };
static const rp_msi_wcolumn_t customaction_cols[] = { { "Action", KEY_S(72) }, { "Type", I2 }, { "Source", S_N(72) },
                                                      { "Target", S_N(255) } };
static const rp_msi_wcolumn_t sequence_cols[] = { { "Action", KEY_S(72) }, { "Condition", S_N(255) }, { "Sequence", I2_N } };

// ---- directories and short names -------------------------------------------------------------

typedef struct {
    char *key;
    char *parent;           // NULL for TARGETDIR
    char *logical;          // stable path text used for derived keys
    char *long_name;        // NULL for TARGETDIR and standard folders
    char *short_name;
} dnode_t;

typedef struct {
    keep_t        *k;
    dnode_t       *v;
    size_t         count, cap;
    const rp_ir_t *ir;
} dirs_t;

static dnode_t *find_node(dirs_t *d, const char *key) {
    for (size_t i = 0; i < d->count; ++i) {
        if (strcmp(d->v[i].key, key) == 0) return &d->v[i];
    }
    return NULL;
}

static dnode_t *add_node(dirs_t *d, char *key, char *parent, char *logical, char *long_name) {
    if (d->count == d->cap) {
        size_t ncap = d->cap ? d->cap * 2 : 16;
        dnode_t *v = rp_mem_alloc(d->k->alloc, ncap, sizeof *v);
        if (v == NULL) {
            d->k->nomem = true;
            return NULL;
        }
        if (d->count) memcpy(v, d->v, d->count * sizeof *v);
        rp_mem_free(d->k->alloc, d->v);
        d->v = v;
        d->cap = ncap;
    }
    dnode_t *n = &d->v[d->count++];
    *n = (dnode_t){ key, parent, logical, long_name, NULL };
    return n;
}

static const char *standard_folder(const char *base, rp_arch_t arch) {
    bool w64 = arch != RP_ARCH_X86;
    static const char *const map[][3] = {
        { "ProgramFiles", "ProgramFiles64Folder", "ProgramFilesFolder" },
        { "ProgramFiles32", "ProgramFilesFolder", "ProgramFilesFolder" },
        { "CommonFiles", "CommonFiles64Folder", "CommonFilesFolder" },
        { "AppData", "AppDataFolder", "AppDataFolder" },
        { "LocalAppData", "LocalAppDataFolder", "LocalAppDataFolder" },
        { "CommonAppData", "CommonAppDataFolder", "CommonAppDataFolder" },
        { "StartMenu", "StartMenuFolder", "StartMenuFolder" },
        { "Programs", "ProgramMenuFolder", "ProgramMenuFolder" },
        { "Desktop", "DesktopFolder", "DesktopFolder" },
        { "Windows", "WindowsFolder", "WindowsFolder" },
        { "System", "System64Folder", "SystemFolder" },
        { "Fonts", "FontsFolder", "FontsFolder" },
        { "Temp", "TempFolder", "TempFolder" },
    };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; ++i) {
        if (strcmp(base, map[i][0]) == 0) return map[i][w64 ? 1 : 2];
    }
    return NULL;
}

// Logical path of an IR dir's final folder: "<standard folder>/<part>/..." (recursive).
static char *ir_dir_logical(dirs_t *d, const rp_ir_dir_t *dir, size_t depth) {
    if (depth > d->ir->dir_count) return NULL;
    const char *head;
    if (dir->base) {
        head = standard_folder(dir->base, d->ir->arch);
    } else {
        const rp_ir_dir_t *p = NULL;
        for (size_t i = 0; i < d->ir->dir_count; ++i) {
            if (strcmp(d->ir->dirs[i].id, dir->parent) == 0) p = &d->ir->dirs[i];
        }
        head = p ? ir_dir_logical(d, p, depth + 1) : NULL;
    }
    if (head == NULL) return NULL;
    char *s = kdup(d->k, head);
    for (size_t i = 0; s && i < dir->part_count; ++i) s = kprintf(d->k, "%s/%s", s, dir->parts[i]);
    return s;
}

static bool valid_83(const char *name) {
    size_t n = strlen(name);
    const char *dot = strchr(name, '.');
    if (n == 0 || n > 12 || (dot && strchr(dot + 1, '.'))) return false;
    size_t base = dot ? (size_t)(dot - name) : n, ext = dot ? n - base - 1 : 0;
    if (base == 0 || base > 8 || ext > 3 || (dot && ext == 0)) return false;
    for (size_t i = 0; i < n; ++i) {
        char c = name[i];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
        if (!ok) return false;
    }
    return true;
}

static bool same_ci(const char *a, const char *b) {
    for (; *a && *b; ++a, ++b) {
        char x = *a, y = *b;
        if (x >= 'a' && x <= 'z') x = (char)(x - 32);
        if (y >= 'a' && y <= 'z') y = (char)(y - 32);
        if (x != y) return false;
    }
    return *a == *b;
}

typedef struct {
    const char *long_name;
    char      **short_out;
} sib_t;

// Short names for one folder's children, independent of source order: children sorted by long
// name; a valid 8.3 long name is its own short name; others get STEM~N.EXT, unique in the folder.
static void assign_short(keep_t *k, sib_t *s, size_t n) {
    for (size_t i = 1; i < n; ++i) {
        for (size_t j = i; j > 0 && strcmp(s[j - 1].long_name, s[j].long_name) > 0; --j) {
            sib_t t = s[j];
            s[j] = s[j - 1];
            s[j - 1] = t;
        }
    }
    for (size_t i = 0; i < n; ++i) *s[i].short_out = valid_83(s[i].long_name) ? kdup(k, s[i].long_name) : NULL;
    for (size_t i = 0; i < n; ++i) {
        if (*s[i].short_out) continue;
        const char *nm = s[i].long_name, *dot = strrchr(nm, '.');
        char stem[8] = "", ext[4] = "";
        size_t sn = 0, en = 0;
        for (const char *p = nm; *p && p != dot && sn < 6; ++p) {
            char c = *p;
            if (c >= 'a' && c <= 'z') c = (char)(c - 32);
            if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-') stem[sn++] = c;
        }
        if (dot) {
            for (const char *p = dot + 1; *p && en < 3; ++p) {
                char c = *p;
                if (c >= 'a' && c <= 'z') c = (char)(c - 32);
                if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-') ext[en++] = c;
            }
        }
        if (sn == 0) {
            memcpy(stem, "RP", 3);
            sn = 2;
        }
        stem[sn] = '\0';
        ext[en] = '\0';
        for (unsigned num = 1; num < 1000000; ++num) {
            char cand[16], digits[8];
            int dn = snprintf(digits, sizeof digits, "%u", num);
            size_t keepn = sn;
            if (keepn + 1 + (size_t)dn > 8) keepn = 8 - 1 - (size_t)dn;
            snprintf(cand, sizeof cand, "%.*s~%s%s%s", (int)keepn, stem, digits, en ? "." : "", ext);
            bool clash = false;
            for (size_t j = 0; j < n && !clash; ++j) {
                if (j != i && *s[j].short_out && same_ci(*s[j].short_out, cand)) clash = true;
            }
            if (!clash) {
                *s[i].short_out = kdup(k, cand);
                break;
            }
        }
    }
}

// ---- lowering --------------------------------------------------------------------------------

typedef struct {
    const rp_ir_file_t *f;
    char               *key;
    char               *comp;
    char               *dir_key;
    char               *short_name;
    uint8_t            *data;
    size_t              size;
} lfile_t;

static int cmp_keys(const lfile_t *a, const lfile_t *b) { return strcmp(a->key, b->key); }

static const char *arch_text(rp_arch_t a) { return a == RP_ARCH_X64 ? "x64" : a == RP_ARCH_ARM64 ? "arm64" : "x86"; }

static bool is_ascii(const char *s) {
    for (; s && *s; ++s) {
        if ((unsigned char)*s >= 0x80) return false;
    }
    return true;
}

// Text that goes into a Formatted column shows brackets literally (RFC-0002 6).
static char *escape_formatted(keep_t *k, const char *s) {
    rp_buf_t b = rp_buf_new(k->alloc, 1u << 20);
    for (; *s; ++s) {
        if (*s == '[') rp_buf_puts(&b, "[\\[]");
        else if (*s == ']') rp_buf_puts(&b, "[\\]]");
        else rp_buf_byte(&b, (uint8_t)*s);
    }
    rp_buf_byte(&b, 0);
    uint8_t *out;
    size_t n;
    if (rp_buf_take(&b, &out, &n) != PROVEN_OK) return keep(k, NULL);
    return keep(k, (char *)out);
}

static proven_err_t write_package(proven_allocator_t alloc, const rp_ir_t *ir, keep_t *k, lfile_t *files, size_t nfiles,
                                  dirs_t *dirs, const char *product_code, const char *package_code,
                                  const rp_limits_t *limits, uint8_t **out, size_t *len) {
    rows_t property, directory, component, feature, featurecomp, file, filehash, media, upgrade, customaction, iexec, iui;
    rows_init(&property, alloc, "Property", property_cols, 2);
    rows_init(&directory, alloc, "Directory", directory_cols, 3);
    rows_init(&component, alloc, "Component", component_cols, 6);
    rows_init(&feature, alloc, "Feature", feature_cols, 8);
    rows_init(&featurecomp, alloc, "FeatureComponents", featurecomp_cols, 2);
    rows_init(&file, alloc, "File", file_cols, 8);
    rows_init(&filehash, alloc, "MsiFileHash", filehash_cols, 6);
    rows_init(&media, alloc, "Media", media_cols, 6);
    rows_init(&upgrade, alloc, "Upgrade", upgrade_cols, 7);
    rows_init(&customaction, alloc, "CustomAction", customaction_cols, 4);
    rows_init(&iexec, alloc, "InstallExecuteSequence", sequence_cols, 3);
    rows_init(&iui, alloc, "InstallUISequence", sequence_cols, 3);
    rows_t *all[] = { &property, &directory, &component, &feature, &featurecomp, &file, &filehash, &media,
                      &upgrade, &customaction, &iexec, &iui };

    // Property
    char version3[24];
    snprintf(version3, sizeof version3, "%u.%u.%u", ir->version_parts[0], ir->version_parts[1], ir->version_parts[2]);
    char lang[8];
    snprintf(lang, sizeof lang, "%u", ir->language);
    s_(&property, "ProductCode"); s_(&property, product_code);
    s_(&property, "ProductName"); s_(&property, ir->name);
    s_(&property, "ProductVersion"); s_(&property, ir->version);
    s_(&property, "Manufacturer"); s_(&property, ir->manufacturer);
    s_(&property, "ProductLanguage"); s_(&property, kdup(k, lang));
    s_(&property, "UpgradeCode"); s_(&property, ir->upgrade_code);
    s_(&property, "ALLUSERS"); s_(&property, "1");
    if (ir->reboot_suppress) { s_(&property, "REBOOT"); s_(&property, "ReallySuppress"); }
    s_(&property, "SecureCustomProperties"); s_(&property, "RP_NEWER_FOUND;RP_OLDER_FOUND");

    // Directory
    for (size_t i = 0; i < dirs->count; ++i) {
        dnode_t *n = &dirs->v[i];
        s_(&directory, n->key);
        s_(&directory, n->parent);
        if (n->parent == NULL) s_(&directory, "SourceDir");
        else if (n->long_name == NULL) s_(&directory, ".");
        else if (strcmp(n->short_name, n->long_name) == 0) s_(&directory, n->long_name);
        else s_(&directory, kprintf(k, "%s|%s", n->short_name, n->long_name));
    }

    // Features (Display: 0 hidden, then odd numbers in ID order)
    int display = 1;
    for (size_t i = 0; i < ir->feature_count; ++i) {
        const rp_ir_feature_t *f = &ir->features[i];
        s_(&feature, f->id);
        s_(&feature, f->parent);
        s_(&feature, f->title);
        s_(&feature, f->description);
        i_(&feature, f->hidden ? 0 : display);
        if (!f->hidden) display += 2;
        i_(&feature, f->level);
        null_(&feature);
        i_(&feature, 0);
    }

    // Files, components, hashes (in File key order = sequence order)
    int32_t comp_attr = ir->arch == RP_ARCH_X86 ? 0 : 256;
    for (size_t i = 0; i < nfiles; ++i) {
        lfile_t *lf = &files[i];
        char guid[39];
        if (lf->f->component_guid) {
            snprintf(guid, sizeof guid, "%s", lf->f->component_guid);
        } else {
            const char *logical = kprintf(k, "%s/%s", lf->f->dir, lf->f->name);
            const char *fields[] = { ir->upgrade_code, "machine", arch_text(ir->arch), logical, "file", lf->key };
            rp_uuid_derive("rubrapack.component", fields, 6, guid);
        }
        s_(&component, lf->comp);
        s_(&component, kdup(k, guid));
        s_(&component, lf->dir_key);
        i_(&component, comp_attr);
        null_(&component);
        s_(&component, lf->key);

        s_(&featurecomp, lf->f->feature);
        s_(&featurecomp, lf->comp);

        s_(&file, lf->key);
        s_(&file, lf->comp);
        if (strcmp(lf->short_name, lf->f->name) == 0) s_(&file, lf->f->name);
        else s_(&file, kprintf(k, "%s|%s", lf->short_name, lf->f->name));
        i_(&file, (int32_t)lf->size);
        null_(&file);
        null_(&file);
        i_(&file, lf->f->vital ? 512 : 0);
        i_(&file, (int32_t)(i + 1));

        uint8_t d[16];
        rp_md5(lf->data, lf->size, d);
        s_(&filehash, lf->key);
        i_(&filehash, 0);
        for (int p = 0; p < 4; ++p) {
            uint32_t w = (uint32_t)d[4 * p] | ((uint32_t)d[4 * p + 1] << 8) | ((uint32_t)d[4 * p + 2] << 16) |
                         ((uint32_t)d[4 * p + 3] << 24);
            i_(&filehash, (int32_t)w);
        }
    }

    // Media and the cabinet
    uint8_t *cab = NULL;
    size_t cab_len = 0;
    proven_err_t err = PROVEN_OK;
    i_(&media, 1);
    i_(&media, (int32_t)nfiles);
    null_(&media);
    if (nfiles) s_(&media, "#cab1.cab");
    else null_(&media);
    null_(&media);
    null_(&media);
    if (nfiles) {
        rp_cab_file_t *cf = rp_mem_alloc(alloc, nfiles, sizeof *cf);
        if (cf == NULL) {
            err = PROVEN_ERR_NOMEM;
        } else {
            for (size_t i = 0; i < nfiles; ++i) cf[i] = (rp_cab_file_t){ files[i].key, files[i].data, files[i].size };
            err = rp_cab_write(alloc, cf, nfiles, ir->compress, limits, &cab, &cab_len);
            rp_mem_free(alloc, cf);
        }
    }

    // Upgrade: detect same-or-newer (refused below), remove older (RFC-0001 9.5)
    s_(&upgrade, ir->upgrade_code); s_(&upgrade, ir->version); null_(&upgrade); null_(&upgrade);
    i_(&upgrade, 0x002 | 0x100); null_(&upgrade); s_(&upgrade, "RP_NEWER_FOUND");
    s_(&upgrade, ir->upgrade_code); null_(&upgrade); s_(&upgrade, ir->version); null_(&upgrade);
    i_(&upgrade, 0x001); null_(&upgrade); s_(&upgrade, "RP_OLDER_FOUND");

    const char *message = ir->downgrade_message ? escape_formatted(k, ir->downgrade_message)
                          : ir->language == 1042 ? "더 새 판이나 같은 판의 [ProductName]이(가) 이미 설치되어 있습니다."
                                                 : "The same or a newer version of [ProductName] is already installed.";
    s_(&customaction, "RP_RefuseDowngrade"); i_(&customaction, 19); null_(&customaction); s_(&customaction, message);

    static const struct { const char *action; const char *cond; int seq; } exec[] = {
        { "FindRelatedProducts", NULL, 25 }, { "RP_RefuseDowngrade", "RP_NEWER_FOUND", 30 },
        { "CostInitialize", NULL, 800 }, { "FileCost", NULL, 900 }, { "CostFinalize", NULL, 1000 },
        { "MigrateFeatureStates", NULL, 1200 }, { "InstallValidate", NULL, 1400 },
        { "InstallInitialize", NULL, 1500 }, { "RemoveExistingProducts", NULL, 1501 },
        { "ProcessComponents", NULL, 1600 }, { "UnpublishFeatures", NULL, 1800 }, { "RemoveFiles", NULL, 3500 },
        { "RemoveFolders", NULL, 3600 }, { "CreateFolders", NULL, 3700 }, { "InstallFiles", NULL, 4000 },
        { "RegisterUser", NULL, 6000 }, { "RegisterProduct", NULL, 6100 }, { "PublishFeatures", NULL, 6300 },
        { "PublishProduct", NULL, 6400 }, { "InstallFinalize", NULL, 6600 },
    };
    for (size_t i = 0; i < sizeof exec / sizeof exec[0]; ++i) {
        s_(&iexec, exec[i].action); s_(&iexec, exec[i].cond); i_(&iexec, exec[i].seq);
    }
    static const struct { const char *action; const char *cond; int seq; } ui[] = {
        { "FindRelatedProducts", NULL, 25 }, { "RP_RefuseDowngrade", "RP_NEWER_FOUND", 30 },
        { "CostInitialize", NULL, 800 }, { "FileCost", NULL, 900 }, { "CostFinalize", NULL, 1000 },
        { "MigrateFeatureStates", NULL, 1200 }, { "ExecuteAction", NULL, 1300 },
    };
    for (size_t i = 0; i < sizeof ui / sizeof ui[0]; ++i) {
        s_(&iui, ui[i].action); s_(&iui, ui[i].cond); i_(&iui, ui[i].seq);
    }

    // Summary information: ASCII strings only, no code page (DECISIONS P1a/P1b).
    const char *subject = ir->summary_name ? ir->summary_name : is_ascii(ir->name) ? ir->name : "rubrapack package";
    const char *author = is_ascii(ir->manufacturer) ? ir->manufacturer : "rubrapack";
    char tmpl[32];
    snprintf(tmpl, sizeof tmpl, "%s;%u", ir->arch == RP_ARCH_X64 ? "x64" : ir->arch == RP_ARCH_ARM64 ? "Arm64" : "Intel",
             ir->language);
    rp_suminfo_t si = { .count = 0 };
    const char *app = "rubrapack " RUBRAPACK_VERSION_STRING;
    struct { uint32_t pid; const char *s; int32_t i; } props[] = {
        { 2, "Installation Database", 0 }, { 3, subject, 0 }, { 4, author, 0 }, { 5, "Installer", 0 },
        { 7, tmpl, 0 }, { 9, package_code, 0 }, { 14, NULL, ir->arch == RP_ARCH_ARM64 ? 500 : 200 },
        { 15, NULL, 2 }, { 18, app, 0 },
    };
    for (size_t i = 0; i < sizeof props / sizeof props[0]; ++i) {
        rp_suminfo_prop_t *p = &si.props[si.count++];
        p->pid = props[i].pid;
        if (props[i].s) {
            p->type = RP_VT_LPSTR;
            p->str = (const uint8_t *)props[i].s;
            p->str_len = strlen(props[i].s);
        } else {
            p->type = RP_VT_I4;
            p->i = props[i].i;
        }
    }
    uint8_t *summary = NULL;
    size_t summary_len = 0;
    if (err == PROVEN_OK) err = rp_suminfo_write(&si, false, alloc, &summary, &summary_len);

    bool nomem = k->nomem;
    for (size_t i = 0; i < sizeof all / sizeof all[0]; ++i) {
        rows_finish(all[i]);
        nomem |= all[i]->nomem;
    }
    if (err == PROVEN_OK && nomem) err = PROVEN_ERR_NOMEM;
    if (err == PROVEN_OK) {
        rp_msi_wtable_t tables[sizeof all / sizeof all[0]];
        for (size_t i = 0; i < sizeof all / sizeof all[0]; ++i) tables[i] = all[i]->t;
        rp_msi_wstream_t cabstream = { "cab1.cab", cab, cab_len };
        rp_msi_wdb_t db = { 65001, tables, sizeof all / sizeof all[0], summary, summary_len, &cabstream, nfiles ? 1 : 0 };
        err = rp_msi_write(alloc, &db, 12, limits, out, len);
    }
    for (size_t i = 0; i < sizeof all / sizeof all[0]; ++i) rp_mem_free(alloc, all[i]->cells);
    rp_mem_free(alloc, cab);
    rp_mem_free(alloc, summary);
    return err;
}

proven_err_t rp_msi_from_ir(proven_allocator_t alloc, const rp_ir_t *ir, const rp_build_options_t *opt,
                            const rp_limits_t *limits, uint8_t **out, size_t *len, rp_srcdiags_t *diags) {
    if (ir == NULL || opt == NULL || limits == NULL || out == NULL || len == NULL || diags == NULL) return PROVEN_ERR_INVALID_ARG;
    keep_t k = { .alloc = alloc };
    dirs_t dirs = { .k = &k, .ir = ir };
    lfile_t *files = rp_mem_alloc(alloc, ir->file_count, sizeof *files);
    proven_err_t err = files ? PROVEN_OK : PROVEN_ERR_NOMEM;
    if (files) memset(files, 0, ir->file_count * sizeof *files);

    // Directory nodes: TARGETDIR, the standard folders used, then each IR dir's path; a node's
    // key is the dir ID for a dir's own folder and D_<hash of its logical path> in between.
    add_node(&dirs, "TARGETDIR", NULL, "TARGETDIR", NULL);
    char **final_logical = rp_mem_alloc(alloc, ir->dir_count, sizeof *final_logical);
    if (final_logical == NULL) err = PROVEN_ERR_NOMEM;
    for (size_t i = 0; err == PROVEN_OK && i < ir->dir_count; ++i) final_logical[i] = ir_dir_logical(&dirs, &ir->dirs[i], 0);
    for (size_t i = 0; err == PROVEN_OK && i < ir->dir_count; ++i) {
        const rp_ir_dir_t *d = &ir->dirs[i];
        if (final_logical[i] == NULL) {
            err = PROVEN_ERR_INVALID_ARG;
            break;
        }
        // Walk from the standard folder down, creating nodes that do not exist yet.
        char *first = kdup(&k, final_logical[i]);
        if (first == NULL) break;
        char *slash = strchr(first, '/');
        if (slash) *slash = '\0';
        if (find_node(&dirs, first) == NULL) add_node(&dirs, first, "TARGETDIR", first, NULL);
        const char *parent = first;
        const char *p = final_logical[i] + strlen(first);
        while (*p == '/') {
            const char *next = strchr(p + 1, '/');
            size_t upto = next ? (size_t)(next - final_logical[i]) : strlen(final_logical[i]);
            char *logical = keep(&k, rp_mem_alloc(alloc, upto + 1, 1));
            if (logical == NULL) break;
            memcpy(logical, final_logical[i], upto);
            logical[upto] = '\0';
            const char *name_start = p + 1;
            size_t name_len = next ? (size_t)(next - name_start) : strlen(name_start);
            char *name = keep(&k, rp_mem_alloc(alloc, name_len + 1, 1));
            if (name == NULL) break;
            memcpy(name, name_start, name_len);
            name[name_len] = '\0';
            // Is this logical folder some dir's own folder?
            const char *key = NULL;
            for (size_t j = 0; j < ir->dir_count; ++j) {
                if (final_logical[j] && strcmp(final_logical[j], logical) == 0) key = ir->dirs[j].id;
            }
            char derived[23];
            if (key == NULL) {
                rp_key_derive('D', logical, derived);
                key = derived;
            }
            if (find_node(&dirs, key) == NULL) add_node(&dirs, kdup(&k, key), (char *)parent, logical, name);
            parent = find_node(&dirs, key) ? find_node(&dirs, key)->key : parent;
            p = next ? next : p + strlen(p);
        }
        (void)d;
    }

    // Files: keys, components, contents.
    for (size_t i = 0; err == PROVEN_OK && i < ir->file_count; ++i) {
        const rp_ir_file_t *f = &ir->files[i];
        lfile_t *lf = &files[i];
        lf->f = f;
        lf->key = kdup(&k, f->id);
        char comp[23];
        rp_key_derive('C', f->id, comp);
        lf->comp = kdup(&k, comp);
        lf->dir_key = kdup(&k, f->dir);
        size_t n = 0;
        if (rp_pal_read_file(alloc, f->source_path, limits->max_output, &lf->data, &n) != PROVEN_OK) {
            rp_srcdiag_add(diags, f->pos, "RP1509", false, "cannot read source file '%s'", f->source);
            err = PROVEN_ERR_IO;
            break;
        }
        lf->size = n;
        if (n > INT32_MAX) {
            rp_srcdiag_add(diags, f->pos, "RP1509", false, "source file '%s' is larger than 2 GiB", f->source);
            err = PROVEN_ERR_OUT_OF_BOUNDS;
        }
    }
    // Sequence order = File key byte order.
    for (size_t i = 1; err == PROVEN_OK && i < ir->file_count; ++i) {
        for (size_t j = i; j > 0 && cmp_keys(&files[j - 1], &files[j]) > 0; --j) {
            lfile_t t = files[j];
            files[j] = files[j - 1];
            files[j - 1] = t;
        }
    }

    // Short names per folder: its sub folders and its files share one namespace.
    for (size_t i = 0; err == PROVEN_OK && i < dirs.count; ++i) {
        const char *folder = dirs.v[i].key;
        size_t n = 0;
        for (size_t j = 0; j < dirs.count; ++j) n += dirs.v[j].parent && dirs.v[j].long_name && strcmp(dirs.v[j].parent, folder) == 0;
        for (size_t j = 0; j < ir->file_count; ++j) n += strcmp(files[j].dir_key, folder) == 0;
        if (n == 0) continue;
        sib_t *s = rp_mem_alloc(alloc, n, sizeof *s);
        if (s == NULL) {
            err = PROVEN_ERR_NOMEM;
            break;
        }
        size_t m = 0;
        for (size_t j = 0; j < dirs.count; ++j) {
            if (dirs.v[j].parent && dirs.v[j].long_name && strcmp(dirs.v[j].parent, folder) == 0) {
                s[m++] = (sib_t){ dirs.v[j].long_name, &dirs.v[j].short_name };
            }
        }
        for (size_t j = 0; j < ir->file_count; ++j) {
            if (strcmp(files[j].dir_key, folder) == 0) s[m++] = (sib_t){ files[j].f->name, &files[j].short_name };
        }
        assign_short(&k, s, m);
        rp_mem_free(alloc, s);
    }

    // Identity.
    char product_code[39], package_code[39];
    if (err == PROVEN_OK) {
        if (ir->product_code) {
            snprintf(product_code, sizeof product_code, "%s", ir->product_code);
        } else {
            char v3[24];
            snprintf(v3, sizeof v3, "%u.%u.%u", ir->version_parts[0], ir->version_parts[1], ir->version_parts[2]);
            const char *fields[] = { ir->upgrade_code, arch_text(ir->arch), "machine", v3 };
            rp_uuid_derive("rubrapack.product", fields, 4, product_code);
        }
        if (opt->reproducible) {
            // First pass with a zero package code; the package code is derived from those bytes.
            uint8_t *first = NULL;
            size_t first_len = 0;
            err = write_package(alloc, ir, &k, files, ir->file_count, &dirs, product_code,
                                "{00000000-0000-0000-0000-000000000000}", limits, &first, &first_len);
            if (err == PROVEN_OK) {
                uint8_t d[PROVEN_SHA256_SIZE];
                proven_sha256((proven_mem_view_t){ first, first_len }, d);
                char hex[65];
                for (int i = 0; i < 32; ++i) snprintf(hex + 2 * i, 3, "%02x", d[i]);
                const char *fields[] = { hex };
                rp_uuid_derive("rubrapack.package", fields, 1, package_code);
                rp_mem_free(alloc, first);
            }
        } else {
            err = rp_uuid_random(package_code);
        }
    }
    if (err == PROVEN_OK) {
        err = write_package(alloc, ir, &k, files, ir->file_count, &dirs, product_code, package_code, limits, out, len);
    }
    if (err == PROVEN_OK && ir->summary_name == NULL && !is_ascii(ir->name)) {
        rp_srcdiag_add(diags, (rp_pos_t){ 1, 1 }, "RP2001", true,
                       "name is not ASCII and summary-name is missing; the summary Subject will read 'rubrapack package'");
    }

    for (size_t i = 0; files && i < ir->file_count; ++i) rp_mem_free(alloc, files[i].data);
    rp_mem_free(alloc, files);
    rp_mem_free(alloc, final_logical);
    rp_mem_free(alloc, dirs.v);
    keep_free(&k);
    if (err == PROVEN_OK && k.nomem) err = PROVEN_ERR_NOMEM;
    return err;
}
