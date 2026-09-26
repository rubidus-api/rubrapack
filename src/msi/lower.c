// src/msi/lower.c - IR -> MSI tables, cabinet and summary (include/rubrapack/build.h).
//
// Identity rules: DECISIONS 2026-09-26 "P2 identity rules". Column types are the ones msi.dll
// writes for the standard tables (P1a fixtures). One component per file, key path = the file.

#include "rubrapack/buf.h"
#include "rubrapack/build.h"
#include "rubrapack/cab.h"
#include "rubrapack/ident.h"
#include "rubrapack/lint.h"
#include "rubrapack/md5.h"
#include "rubrapack/mem.h"
#include "rubrapack/msi.h"
#include "rubrapack/pal.h"
#include "rubrapack/pe.h"
#include "rubrapack/suminfo.h"
#include "rubrapack/text.h"
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
static const rp_msi_wcolumn_t createfolder_cols[] = { { "Directory_", KEY_S(72) }, { "Component_", KEY_S(72) } };
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
static const rp_msi_wcolumn_t registry_cols[] = { { "Registry", KEY_S(72) }, { "Root", I2 }, { "Key", L(255) },
                                                  { "Name", L_N(255) }, { "Value", L_N(0) }, { "Component_", S(72) } };
static const rp_msi_wcolumn_t removereg_cols[] = { { "RemoveRegistry", KEY_S(72) }, { "Root", I2 }, { "Key", L(255) },
                                                   { "Name", L_N(255) }, { "Component_", S(72) } };
static const rp_msi_wcolumn_t shortcut_cols[] = { { "Shortcut", KEY_S(72) }, { "Directory_", S(72) }, { "Name", L(128) },
                                                  { "Component_", S(72) }, { "Target", S(72) }, { "Arguments", S_N(255) },
                                                  { "Description", L_N(255) }, { "Hotkey", I2_N }, { "Icon_", S_N(72) },
                                                  { "IconIndex", I2_N }, { "ShowCmd", I2_N }, { "WkDir", S_N(72) } };
static const rp_msi_wcolumn_t removefile_cols[] = { { "FileKey", KEY_S(72) }, { "Component_", S(72) }, { "FileName", L_N(255) },
                                                    { "DirProperty", S(72) }, { "InstallMode", I2 } };
static const rp_msi_wcolumn_t duplicate_cols[] = { { "FileKey", KEY_S(72) }, { "Component_", S(72) }, { "File_", S(72) },
                                                   { "DestName", L_N(255) }, { "DestFolder", S_N(72) } };
static const rp_msi_wcolumn_t environment_cols[] = { { "Environment", KEY_S(72) }, { "Name", L(255) }, { "Value", L_N(255) },
                                                     { "Component_", S(72) } };
static const rp_msi_wcolumn_t sequence_cols[] = { { "Action", KEY_S(72) }, { "Condition", S_N(255) }, { "Sequence", I2_N } };

// ---- directories and short names -------------------------------------------------------------

typedef struct {
    char *key;
    char *parent;           // NULL for TARGETDIR
    char *logical;          // stable path text used for derived keys
    char *long_name;        // NULL for TARGETDIR and standard folders
    char *short_name;
    rp_pos_t pos;           // the [dir.*] or [folder.*] that made it (for diagnostics)
} dnode_t;

typedef struct {
    keep_t        *k;
    dnode_t       *v;
    size_t         count, cap;
    const rp_ir_t *ir;
    char         **sc_short;    // short names of ir->shortcuts, same order
    char         **cp_short;    // short names of ir->copies, same order
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
    *n = (dnode_t){ key, parent, logical, long_name, NULL, { 0, 0 } };
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
        { "Startup", "StartupFolder", "StartupFolder" },
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

// `SHORT|Long` (or Long alone when it is its own short name) must fit a `width`-wide column
// (255; Shortcut.Name 128). Returns 0 when it fits, else the long name's length; *most is the
// longest name that would fit.
static size_t name_cell_excess_w(const char *short_name, const char *long_name, size_t width, size_t *most) {
    rp_text_result_t a = rp_utf8_to_utf16((const uint8_t *)long_name, strlen(long_name), NULL, 0);
    size_t extra = strcmp(short_name, long_name) != 0 ? strlen(short_name) + 1 : 0;   // ASCII short name and '|'
    *most = width - extra;
    return a.err == PROVEN_OK && a.units + extra <= width ? 0 : a.units;
}

static size_t name_cell_excess(const char *short_name, const char *long_name, size_t *most) {
    return name_cell_excess_w(short_name, long_name, 255, most);
}

// The Directory key a shortcut goes to: a known folder's standard property, or its dir ID.
static const char *shortcut_dir_key(const rp_ir_shortcut_t *sc, rp_arch_t arch) {
    const char *std = standard_folder(sc->dir, arch);
    bool known = strcmp(sc->dir, "Programs") == 0 || strcmp(sc->dir, "Desktop") == 0 || strcmp(sc->dir, "StartMenu") == 0 ||
                 strcmp(sc->dir, "Startup") == 0;
    return known && std ? std : sc->dir;
}

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
                                  const rp_limits_t *limits, uint8_t **out, size_t *len, rp_srcdiags_t *diags) {
    rows_t property, directory, component, feature, featurecomp, file, filehash, media, upgrade, customaction, iexec, iui,
        createfolder, registry, removereg, shortcut, removefile, duplicate, environment;
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
    rows_init(&createfolder, alloc, "CreateFolder", createfolder_cols, 2);
    rows_init(&registry, alloc, "Registry", registry_cols, 6);
    rows_init(&removereg, alloc, "RemoveRegistry", removereg_cols, 5);
    rows_init(&shortcut, alloc, "Shortcut", shortcut_cols, 12);
    rows_init(&removefile, alloc, "RemoveFile", removefile_cols, 5);
    rows_init(&duplicate, alloc, "DuplicateFile", duplicate_cols, 5);
    rows_init(&environment, alloc, "Environment", environment_cols, 4);
    // The P3 tables are written only when they have rows, so packages without them stay as they were.
    rows_t *all[] = { &property, &directory, &component, &feature, &featurecomp, &file, &filehash, &media,
                      &upgrade, &customaction, &iexec, &iui, &createfolder, &registry, &removereg, &shortcut,
                      &removefile, &duplicate, &environment };
    const size_t always = 13;

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
    // Never close the user's programs to free a file (RFC-0003 X4): at /qn Restart Manager shuts
    // down every process holding a file, and fails the installation if one does not close. It is
    // authored here, not passed on the command line, so the old package's removal inside an
    // upgrade obeys it too (observed).
    s_(&property, "MSIRESTARTMANAGERCONTROL"); s_(&property, "Disable");
    // [arp] and [property.*] (RFC-0003 1). Secure and hidden properties are listed for the engine.
    if (ir->arp_no_modify) { s_(&property, "ARPNOMODIFY"); s_(&property, "1"); }
    if (ir->arp_no_repair) { s_(&property, "ARPNOREPAIR"); s_(&property, "1"); }
    if (ir->arp_help) { s_(&property, "ARPHELPLINK"); s_(&property, ir->arp_help); }
    if (ir->arp_about) { s_(&property, "ARPURLINFOABOUT"); s_(&property, ir->arp_about); }
    char *secure = kdup(k, ir->refuse_below ? "RP_NEWER_FOUND;RP_OLDER_FOUND;RP_REFUSED_OLD" : "RP_NEWER_FOUND;RP_OLDER_FOUND");
    char *hidden = NULL;
    for (size_t i = 0; i < ir->property_count; ++i) {
        const rp_ir_property_t *p = &ir->properties[i];
        s_(&property, p->id);
        s_(&property, p->value);
        if (p->secure) secure = kprintf(k, "%s;%s", secure, p->id);
        if (p->hidden) hidden = hidden ? kprintf(k, "%s;%s", hidden, p->id) : kdup(k, p->id);
    }
    s_(&property, "SecureCustomProperties"); s_(&property, secure);
    if (hidden) { s_(&property, "MsiHiddenProperties"); s_(&property, hidden); }

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
        // Versioned files carry their version and language; the others get a hash row.
        rp_pe_info_t pi;
        bool versioned = rp_pe_read(lf->data, lf->size, &pi) == PROVEN_OK && pi.has_version;
        if (versioned) {
            char vtext[32], ltext[8];
            snprintf(vtext, sizeof vtext, "%u.%u.%u.%u", pi.version[0], pi.version[1], pi.version[2], pi.version[3]);
            snprintf(ltext, sizeof ltext, "%u", pi.language);
            s_(&file, kdup(k, vtext));
            s_(&file, kdup(k, ltext));
        } else {
            null_(&file);
            null_(&file);
        }
        i_(&file, lf->f->vital ? 512 : 0);
        i_(&file, (int32_t)(i + 1));
        if (versioned) continue;

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

    // [registry.*] (RFC-0004): own component per value (key path = the value) unless `with`.
    bool any_write = false, any_remove = false, reg_bad = false;
    for (size_t i = 0; i < ir->registry_count; ++i) {
        const rp_ir_registry_t *r = &ir->registries[i];
        const char *ckey = NULL;
        if (r->with_file) {
            for (size_t j = 0; j < nfiles; ++j) {
                if (strcmp(files[j].key, r->with_file) == 0) ckey = files[j].comp;
            }
        } else {
            char comp[23], guid[39];
            const char *ident = kprintf(k, "registry:%s", r->id, NULL);
            rp_key_derive('C', ident, comp);
            static const char *const roots[] = { "HKCR", "HKCU", "HKLM" };
            const char *logical = kprintf(k, "%s\\%s", roots[r->root], r->key);
            logical = kprintf(k, "%s\\%s", logical, r->name ? r->name : "");
            const char *fields[] = { ir->upgrade_code, "machine", r->view32 ? "x86" : arch_text(ir->arch), logical, "registry", r->id };
            rp_uuid_derive("rubrapack.component", fields, 6, guid);
            ckey = kdup(k, comp);
            int32_t attr = (r->remove ? 0 : 4) | (ir->arch != RP_ARCH_X86 && !r->view32 ? 256 : 0) | (r->keep ? 16 : 0);
            s_(&component, ckey);
            s_(&component, kdup(k, guid));
            s_(&component, "TARGETDIR");
            i_(&component, attr);
            null_(&component);
            if (r->remove) null_(&component);
            else s_(&component, r->id);
            s_(&featurecomp, r->feature);
            s_(&featurecomp, ckey);
        }
        if (ckey == NULL) {
            reg_bad = true;                     // the IR checked `with`; unreachable
            break;
        }
        const char *key = escape_formatted(k, r->key);
        const char *name = r->name ? escape_formatted(k, r->name) : NULL;
        if (r->remove) {
            s_(&removereg, r->id); i_(&removereg, (int32_t)r->root); s_(&removereg, key);
            s_(&removereg, name ? name : "-");  // no name: the whole key
            s_(&removereg, ckey);
            any_remove = true;
            continue;
        }
        const char *value = NULL;
        switch (r->type) {
        case RP_REG_STRING: value = r->value[0] == '#' ? kprintf(k, "#%s", r->value, NULL) : r->value; break;
        case RP_REG_EXPAND: value = kprintf(k, "#%%%s", r->value, NULL); break;
        case RP_REG_DWORD: value = kprintf(k, "#%s", r->value, NULL); break;
        case RP_REG_BINARY: value = kprintf(k, "#x%s", r->value, NULL); break;
        case RP_REG_MULTI:
            value = "[~]";
            for (size_t j = 0; j < r->item_count; ++j) value = kprintf(k, "%s%s[~]", value, r->items[j]);
            break;
        }
        s_(&registry, r->id); i_(&registry, (int32_t)r->root); s_(&registry, key); s_(&registry, name);
        s_(&registry, value); s_(&registry, ckey);
        any_write = true;
    }

    // [shortcut.*] (RFC-0004): in the target file's component; folders made for them are removed
    // again at uninstall (RemoveFile, mode 2), from the shortcut's folder up to the known folder.
    for (size_t i = 0; i < ir->shortcut_count; ++i) {
        const rp_ir_shortcut_t *sc = &ir->shortcuts[i];
        const lfile_t *target = NULL;
        for (size_t j = 0; j < nfiles; ++j) {
            if (strcmp(files[j].key, sc->target_file) == 0) target = &files[j];
        }
        if (target == NULL) {
            reg_bad = true;
            break;
        }
        const char *dkey = shortcut_dir_key(sc, ir->arch);
        const char *shortn = dirs->sc_short[i];
        s_(&shortcut, sc->id);
        s_(&shortcut, dkey);
        s_(&shortcut, strcmp(shortn, sc->name) == 0 ? sc->name : kprintf(k, "%s|%s", shortn, sc->name));
        s_(&shortcut, target->comp);
        s_(&shortcut, kprintf(k, "[#%s]", target->key, NULL));
        s_(&shortcut, sc->args);                                            // formatted (H2)
        s_(&shortcut, sc->description);                                     // Text, not formatted
        null_(&shortcut); null_(&shortcut); null_(&shortcut); null_(&shortcut);
        s_(&shortcut, sc->working_dir);
        for (const dnode_t *n = find_node(dirs, dkey); n && n->long_name && n->parent; n = find_node(dirs, n->parent)) {
            char rk[23];
            rp_key_derive('R', kprintf(k, "shortcut-folder:%s", n->key, NULL), rk);
            bool seen = false;
            for (size_t r = 0; r + 5 <= removefile.filled; r += 5) {
                const rp_msi_cell_t *c0 = &removefile.cells[r];
                seen |= c0->len == strlen(rk) && memcmp(c0->bytes, rk, c0->len) == 0;
            }
            if (seen) continue;
            s_(&removefile, kdup(k, rk)); s_(&removefile, target->comp); null_(&removefile);
            s_(&removefile, n->key); i_(&removefile, 2);
        }
    }

    // [remove.*] (RFC-0004): own component in that folder (no key path file), one RemoveFile row.
    for (size_t i = 0; i < ir->remove_count; ++i) {
        const rp_ir_remove_t *r = &ir->removes[i];
        char comp[23], guid[39];
        rp_key_derive('C', kprintf(k, "remove:%s", r->id, NULL), comp);
        const char *logical = kprintf(k, "%s/%s", r->dir, r->name ? r->name : "");
        const char *fields[] = { ir->upgrade_code, "machine", arch_text(ir->arch), logical, "remove", r->id };
        rp_uuid_derive("rubrapack.component", fields, 6, guid);
        const char *ckey = kdup(k, comp);
        s_(&component, ckey); s_(&component, kdup(k, guid)); s_(&component, r->dir);
        i_(&component, ir->arch != RP_ARCH_X86 ? 256 : 0); null_(&component); null_(&component);
        s_(&featurecomp, r->feature); s_(&featurecomp, ckey);
        s_(&removefile, r->id); s_(&removefile, ckey); s_(&removefile, r->name); s_(&removefile, r->dir);
        i_(&removefile, r->mode);
    }

    // [env.*] (RFC-0004): system variables ('*'), each its own component under TARGETDIR. With '-'
    // the engine undoes it at uninstall: a set variable is deleted, appended or prepended text is
    // taken out and the rest kept; without '-' (keep) nothing is undone (observed).
    for (size_t i = 0; i < ir->env_count; ++i) {
        const rp_ir_env_t *e = &ir->envs[i];
        char comp[23], guid[39];
        rp_key_derive('C', kprintf(k, "env:%s", e->id, NULL), comp);
        const char *fields[] = { ir->upgrade_code, "machine", arch_text(ir->arch), e->name, "env", e->id };
        rp_uuid_derive("rubrapack.component", fields, 6, guid);
        const char *ckey = kdup(k, comp);
        s_(&component, ckey); s_(&component, kdup(k, guid)); s_(&component, "TARGETDIR");
        i_(&component, (ir->arch != RP_ARCH_X86 ? 256 : 0) | (e->keep ? 16 : 0)); null_(&component); null_(&component);
        s_(&featurecomp, e->feature); s_(&featurecomp, ckey);
        const char *prefix = !e->keep ? "=-*" : "=*";
        const char *value = e->mode == 1 ? kprintf(k, "[~];%s", e->value, NULL)
                          : e->mode == 2 ? kprintf(k, "%s;[~]", e->value, NULL)
                                         : e->value;
        s_(&environment, e->id); s_(&environment, kprintf(k, "%s%s", prefix, e->name));
        s_(&environment, value); s_(&environment, ckey);
    }

    // [copy.*] (RFC-0004): DuplicateFile in the source file's component.
    for (size_t i = 0; i < ir->copy_count; ++i) {
        const rp_ir_copy_t *cp = &ir->copies[i];
        const lfile_t *src = NULL;
        for (size_t j = 0; j < nfiles; ++j) {
            if (strcmp(files[j].key, cp->source_file) == 0) src = &files[j];
        }
        if (src == NULL) {
            reg_bad = true;
            break;
        }
        const char *longn = cp->name ? cp->name : src->f->name;
        const char *shortn = dirs->cp_short[i];
        s_(&duplicate, cp->id); s_(&duplicate, src->comp); s_(&duplicate, src->key);
        s_(&duplicate, strcmp(shortn, longn) == 0 ? longn : kprintf(k, "%s|%s", shortn, longn));
        s_(&duplicate, cp->dir);
    }

    // Folders: CreateFolder, one component each, the folder itself as the key path.
    for (size_t i = 0; i < ir->folder_count; ++i) {
        const rp_ir_folder_t *f = &ir->folders[i];
        char comp[23], guid[39];
        rp_key_derive('C', f->id, comp);
        const char *logical = kprintf(k, "%s/%s", f->dir, f->name);
        const char *fields[] = { ir->upgrade_code, "machine", arch_text(ir->arch), logical, "folder", f->id };
        rp_uuid_derive("rubrapack.component", fields, 6, guid);
        const char *ckey = kdup(k, comp);
        s_(&component, ckey);
        s_(&component, kdup(k, guid));
        s_(&component, f->id);
        i_(&component, comp_attr | (f->keep ? 16 : 0));
        null_(&component);
        null_(&component);
        s_(&featurecomp, f->feature);
        s_(&featurecomp, ckey);
        s_(&createfolder, f->id);
        s_(&createfolder, ckey);
    }

    // Media and the cabinet
    uint8_t *cab = NULL;
    size_t cab_len = 0;
    proven_err_t err = reg_bad ? PROVEN_ERR_INVALID_ARG : PROVEN_OK;
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
    if (ir->refuse_below) {         // detect only (0x002), below the given version (max exclusive)
        s_(&upgrade, ir->upgrade_code); null_(&upgrade); s_(&upgrade, ir->refuse_below); null_(&upgrade);
        i_(&upgrade, 0x002); null_(&upgrade); s_(&upgrade, "RP_REFUSED_OLD");
    }

    const char *message = ir->downgrade_message ? escape_formatted(k, ir->downgrade_message)
                          : ir->language == 1042 ? "더 새 판이나 같은 판의 [ProductName]이(가) 이미 설치되어 있습니다."
                                                 : "The same or a newer version of [ProductName] is already installed.";
    s_(&customaction, "RP_RefuseDowngrade"); i_(&customaction, 19); null_(&customaction); s_(&customaction, message);
    // RFC-0003 section 9 (T1): an old version that must be removed by hand first. The removal command
    // names the found product ([RP_REFUSED_OLD]) and keeps Restart Manager off for that removal.
    if (ir->refuse_below) {
        const char *cmd = "msiexec /x [RP_REFUSED_OLD] /qn MSIRESTARTMANAGERCONTROL=Disable";
        const char *text = ir->refuse_message ? escape_formatted(k, ir->refuse_message)
                           : ir->language == 1042 ? "설치된 옛 판 [ProductName]은(는) 이 설치로 올릴 수 없습니다. 먼저 지운 뒤 다시 설치하십시오:"
                                                  : "The installed older version of [ProductName] cannot be upgraded by this package. Remove it first, then run this installation again:";
        s_(&customaction, "RP_RefuseOld"); i_(&customaction, 19); null_(&customaction);
        s_(&customaction, kprintf(k, "%s %s", text, cmd));
    }

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
    // [action.*] do/undo pairs (RFC-0003 2). Nothing that writes script may sit between
    // InstallInitialize and RemoveExistingProducts (error 2613 on the VM, lint RP2019).
    size_t na = ir->action_count;
    for (size_t i = 0; i < sizeof exec / sizeof exec[0]; ++i) {
        s_(&iexec, exec[i].action); s_(&iexec, exec[i].cond); i_(&iexec, exec[i].seq);
    }
    if (ir->env_count) {            // MS Learn "Suggested InstallExecuteSequence"
        s_(&iexec, "RemoveEnvironmentStrings"); null_(&iexec); i_(&iexec, 3310);
        s_(&iexec, "WriteEnvironmentStrings"); null_(&iexec); i_(&iexec, 5200);
    }
    if (ir->copy_count) {           // MS Learn "Suggested InstallExecuteSequence" (3400 is used by our Undo pairs)
        s_(&iexec, "RemoveDuplicateFiles"); null_(&iexec); i_(&iexec, 3300);
        s_(&iexec, "DuplicateFiles"); null_(&iexec); i_(&iexec, 4210);
    }
    if (ir->shortcut_count) {       // MS Learn "Suggested InstallExecuteSequence"
        s_(&iexec, "RemoveShortcuts"); null_(&iexec); i_(&iexec, 3200);
        s_(&iexec, "CreateShortcuts"); null_(&iexec); i_(&iexec, 4500);
    }
    if (any_write || any_remove) {  // MS Learn "Suggested InstallExecuteSequence"
        s_(&iexec, "RemoveRegistryValues"); null_(&iexec); i_(&iexec, 2600);
        s_(&iexec, "WriteRegistryValues"); null_(&iexec); i_(&iexec, 5000);
    }
    if (ir->refuse_below) {
        s_(&iexec, "RP_RefuseOld"); s_(&iexec, "RP_REFUSED_OLD"); i_(&iexec, 31);
        s_(&iui, "RP_RefuseOld"); s_(&iui, "RP_REFUSED_OLD"); i_(&iui, 31);
    }
    for (size_t i = 0; i < na; ++i) {
        const rp_ir_action_t *a = &ir->actions[i];
        const lfile_t *run = NULL;
        for (size_t j = 0; j < nfiles; ++j) {
            if (strcmp(files[j].key, a->run_file) == 0) run = &files[j];
        }
        if (run == NULL) {
            err = PROVEN_ERR_INVALID_ARG;       // the IR checked it; unreachable
            break;
        }
        const char *do_args = escape_formatted(k, a->do_args), *undo_args = escape_formatted(k, a->undo_args);
        if (do_args == NULL || undo_args == NULL) break;
        size_t longest = strlen(do_args) > strlen(undo_args) ? strlen(do_args) : strlen(undo_args);
        if (longest > 255) {
            rp_srcdiag_add(diags, a->pos, "RP1313", false, "do/undo arguments are longer than 255 characters");
            err = PROVEN_ERR_INVALID_FORMAT;
            break;
        }
        // $C: what this installation does to the exe's component; ?C: its state before.
        const char *install = kprintf(k, "$%s>2", run->comp, NULL);
        const char *fresh = kprintf(k, "$%s>2 AND ?%s<>3", run->comp, run->comp);
        const char *again = kprintf(k, "$%s>2 AND ?%s=3", run->comp, run->comp);
        const char *remove = kprintf(k, "$%s=2 AND ?%s=3", run->comp, run->comp);
        enum { FORWARD = 18 | 0x400 | 0x800, ROLLBACK = 18 | 0x100 | 0x400 | 0x800 | 0x40 };
        struct { const char *suffix; int type; const char *args; const char *cond; int seq; } rows[] = {
            { "UndoRollback", ROLLBACK, do_args, remove, 3400 + 2 * (int)(na - 1 - i) },
            { "Undo", FORWARD, undo_args, remove, 3401 + 2 * (int)(na - 1 - i) },
            { "DoRollback", ROLLBACK, undo_args, fresh, 4001 + 3 * (int)i },
            { "RedoRollback", ROLLBACK, do_args, again, 4002 + 3 * (int)i },
            { "Do", FORWARD, do_args, install, 4003 + 3 * (int)i },
        };
        for (size_t r = 0; r < sizeof rows / sizeof rows[0]; ++r) {
            const char *name = kprintf(k, "RP_%s_%s", a->id, rows[r].suffix);
            s_(&customaction, name); i_(&customaction, rows[r].type); s_(&customaction, run->key);
            s_(&customaction, rows[r].args);
            s_(&iexec, name); s_(&iexec, rows[r].cond); i_(&iexec, rows[r].seq);
        }
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
        size_t nt = 0;
        for (size_t i = 0; i < sizeof all / sizeof all[0]; ++i) {
            if (i < always || all[i]->t.row_count > 0) tables[nt++] = all[i]->t;
        }
        rp_msi_wstream_t cabstream = { "cab1.cab", cab, cab_len };
        rp_msi_wdb_t db = { 65001, tables, nt, summary, summary_len, &cabstream, nfiles ? 1 : 0 };
        err = rp_msi_lint(alloc, &db, diags);     // RFC-0001 7.1: build always checks what it writes
        if (err == PROVEN_OK) err = rp_msi_write(alloc, &db, 12, limits, out, len);
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
            if (find_node(&dirs, key) && find_node(&dirs, key)->pos.line == 0) find_node(&dirs, key)->pos = d->pos;
            parent = find_node(&dirs, key) ? find_node(&dirs, key)->key : parent;
            p = next ? next : p + strlen(p);
        }
    }

    // Folders to create are directory nodes below their dir.
    for (size_t i = 0; err == PROVEN_OK && i < ir->folder_count; ++i) {
        const rp_ir_folder_t *f = &ir->folders[i];
        const char *parent_logical = NULL;
        for (size_t j = 0; j < ir->dir_count; ++j) {
            if (strcmp(ir->dirs[j].id, f->dir) == 0) parent_logical = final_logical[j];
        }
        if (parent_logical == NULL) {
            err = PROVEN_ERR_INVALID_ARG;
            break;
        }
        add_node(&dirs, kdup(&k, f->id), kdup(&k, f->dir), kprintf(&k, "%s/%s", parent_logical, f->name), kdup(&k, f->name));
        if (find_node(&dirs, f->id)) find_node(&dirs, f->id)->pos = f->pos;
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

    // Shortcuts into a known folder need that folder's Directory row.
    dirs.sc_short = rp_mem_alloc(alloc, ir->shortcut_count + 1, sizeof *dirs.sc_short);
    dirs.cp_short = rp_mem_alloc(alloc, ir->copy_count + 1, sizeof *dirs.cp_short);
    if (dirs.sc_short == NULL || dirs.cp_short == NULL) err = PROVEN_ERR_NOMEM;
    for (size_t i = 0; err == PROVEN_OK && i < ir->copy_count; ++i) dirs.cp_short[i] = NULL;
    for (size_t i = 0; err == PROVEN_OK && i < ir->shortcut_count; ++i) {
        dirs.sc_short[i] = NULL;
        const char *key = shortcut_dir_key(&ir->shortcuts[i], ir->arch);
        if (find_node(&dirs, key) == NULL) add_node(&dirs, kdup(&k, key), "TARGETDIR", kdup(&k, key), NULL);
    }

    // Short names per folder: its sub folders, its files and its shortcuts share one namespace.
    for (size_t i = 0; err == PROVEN_OK && i < dirs.count; ++i) {
        const char *folder = dirs.v[i].key;
        size_t n = 0;
        for (size_t j = 0; j < dirs.count; ++j) n += dirs.v[j].parent && dirs.v[j].long_name && strcmp(dirs.v[j].parent, folder) == 0;
        for (size_t j = 0; j < ir->file_count; ++j) n += strcmp(files[j].dir_key, folder) == 0;
        for (size_t j = 0; j < ir->shortcut_count; ++j) n += strcmp(shortcut_dir_key(&ir->shortcuts[j], ir->arch), folder) == 0;
        for (size_t j = 0; j < ir->copy_count; ++j) n += strcmp(ir->copies[j].dir, folder) == 0;
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
        for (size_t j = 0; j < ir->shortcut_count; ++j) {
            if (strcmp(shortcut_dir_key(&ir->shortcuts[j], ir->arch), folder) == 0) {
                s[m++] = (sib_t){ ir->shortcuts[j].name, &dirs.sc_short[j] };
            }
        }
        for (size_t j = 0; j < ir->copy_count; ++j) {
            if (strcmp(ir->copies[j].dir, folder) != 0) continue;
            const char *longn = ir->copies[j].name;
            for (size_t f = 0; longn == NULL && f < ir->file_count; ++f) {
                if (strcmp(ir->files[f].id, ir->copies[j].source_file) == 0) longn = ir->files[f].name;
            }
            s[m++] = (sib_t){ longn ? longn : ir->copies[j].id, &dirs.cp_short[j] };
        }
        assign_short(&k, s, m);
        rp_mem_free(alloc, s);
    }

    // File.FileName and Directory.DefaultDir hold `SHORT|Long` in 255 UTF-16 units, so a long name
    // that NTFS accepts can still not fit; say so at the source line (lint would only see the cell).
    for (size_t i = 0, units, most; err == PROVEN_OK && i < ir->file_count; ++i) {
        if ((units = name_cell_excess(files[i].short_name, files[i].f->name, &most)) != 0) {
            rp_srcdiag_add(diags, files[i].f->pos, "RP1514", false,
                           "file name is %zu UTF-16 units; with its 8.3 short name MSI holds at most %zu", units, most);
            err = PROVEN_ERR_INVALID_FORMAT;
        }
    }
    for (size_t i = 0, units, most; err == PROVEN_OK && i < ir->shortcut_count; ++i) {
        const rp_ir_shortcut_t *sc = &ir->shortcuts[i];
        if (dirs.sc_short[i] && (units = name_cell_excess_w(dirs.sc_short[i], sc->name, 128, &most)) != 0) {
            rp_srcdiag_add(diags, sc->pos, "RP1514", false,
                           "shortcut name is %zu UTF-16 units; with its 8.3 short name MSI holds at most %zu", units, most);
            err = PROVEN_ERR_INVALID_FORMAT;
        }
    }
    for (size_t i = 0, units, most; err == PROVEN_OK && i < dirs.count; ++i) {
        const dnode_t *n = &dirs.v[i];
        if (n->long_name && n->short_name && (units = name_cell_excess(n->short_name, n->long_name, &most)) != 0) {
            rp_srcdiag_add(diags, n->pos, "RP1514", false,
                           "folder name is %zu UTF-16 units; with its 8.3 short name MSI holds at most %zu", units, most);
            err = PROVEN_ERR_INVALID_FORMAT;
        }
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
                                "{00000000-0000-0000-0000-000000000000}", limits, &first, &first_len, diags);
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
        err = write_package(alloc, ir, &k, files, ir->file_count, &dirs, product_code, package_code, limits, out, len,
                            opt->reproducible ? &(rp_srcdiags_t){ 0 } : diags);  // the first pass reported already
    }
    if (err == PROVEN_OK && ir->summary_name == NULL && !is_ascii(ir->name)) {
        rp_srcdiag_add(diags, (rp_pos_t){ 1, 1 }, "RP1203", true,
                       "name is not ASCII and summary-name is missing; the summary Subject will read 'rubrapack package'");
    }

    for (size_t i = 0; files && i < ir->file_count; ++i) rp_mem_free(alloc, files[i].data);
    rp_mem_free(alloc, files);
    rp_mem_free(alloc, final_logical);
    rp_mem_free(alloc, dirs.sc_short);
    rp_mem_free(alloc, dirs.cp_short);
    rp_mem_free(alloc, dirs.v);
    keep_free(&k);
    if (err == PROVEN_OK && k.nomem) err = PROVEN_ERR_NOMEM;
    return err;
}
