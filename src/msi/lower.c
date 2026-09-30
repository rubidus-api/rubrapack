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
#include "rubrapack/parts.h"
#include "rubrapack/pe.h"
#include "rubrapack/suminfo.h"
#include "rubrapack/text.h"
#include "rubrapack/ui.h"
#include "rubrapack/version.h"

#include <stdio.h>
#include <stdlib.h>
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

static void b_(rows_t *r, const uint8_t *data, size_t len) {  // binary (stream) cell
    rp_msi_cell_t *c = next_cell(r);
    if (c == NULL) return;
    c->kind = RP_MSI_BINARY;
    c->bytes = data;
    c->len = len;
}

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
#define I4_N      0x1104u
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
static const rp_msi_wcolumn_t inifile_cols[] = { { "IniFile", KEY_S(72) }, { "FileName", L(255) }, { "DirProperty", S_N(72) },
                                                 { "Section", L(96) }, { "Key", L(128) }, { "Value", L(255) },
                                                 { "Action", I2 }, { "Component_", S(72) } };
static const rp_msi_wcolumn_t removeini_cols[] = { { "RemoveIniFile", KEY_S(72) }, { "FileName", L(255) },
                                                   { "DirProperty", S_N(72) }, { "Section", L(96) }, { "Key", L(128) },
                                                   { "Value", L_N(255) }, { "Action", I2 }, { "Component_", S(72) } };
static const rp_msi_wcolumn_t launch_cols[] = { { "Condition", KEY_S(255) }, { "Description", L(255) } };
static const rp_msi_wcolumn_t appsearch_cols[] = { { "Property", KEY_S(72) }, { "Signature_", KEY_S(72) } };
static const rp_msi_wcolumn_t reglocator_cols[] = { { "Signature_", KEY_S(72) }, { "Root", I2 }, { "Key", S(255) },
                                                    { "Name", S_N(255) }, { "Type", I2_N } };
static const rp_msi_wcolumn_t drlocator_cols[] = { { "Signature_", KEY_S(72) }, { "Parent", KEY_S_N(72) },
                                                   { "Path", KEY_S_N(255) }, { "Depth", I2_N } };
static const rp_msi_wcolumn_t signature_cols[] = { { "Signature", KEY_S(72) }, { "FileName", S(255) }, { "MinVersion", S_N(20) },
                                                   { "MaxVersion", S_N(20) }, { "MinSize", I4_N }, { "MaxSize", I4_N },
                                                   { "MinDate", I4_N }, { "MaxDate", I4_N }, { "Languages", S_N(255) } };
static const rp_msi_wcolumn_t complocator_cols[] = { { "Signature_", KEY_S(72) }, { "ComponentId", S(38) }, { "Type", I2_N } };
static const rp_msi_wcolumn_t svcinstall_cols[] = { { "ServiceInstall", KEY_S(72) }, { "Name", S(255) }, { "DisplayName", L_N(255) },
                                                    { "ServiceType", I4 }, { "StartType", I4 }, { "ErrorControl", I4 },
                                                    { "LoadOrderGroup", S_N(255) }, { "Dependencies", S_N(255) },
                                                    { "StartName", S_N(255) }, { "Password", S_N(255) },
                                                    { "Arguments", S_N(255) }, { "Component_", S(72) },
                                                    { "Description", L_N(255) } };
static const rp_msi_wcolumn_t svccontrol_cols[] = { { "ServiceControl", KEY_S(72) }, { "Name", L(255) }, { "Event", I2 },
                                                    { "Arguments", S_N(255) }, { "Wait", I2_N }, { "Component_", S(72) } };
static const rp_msi_wcolumn_t font_cols[] = { { "File_", KEY_S(72) }, { "FontTitle", S_N(128) } };
static const rp_msi_wcolumn_t lockperm_cols[] = { { "MsiLockPermissionsEx", KEY_S(72) }, { "LockObject", S(72) },
                                                  { "Table", S(32) }, { "SDDLText", S(0) }, { "Condition", S_N(255) } };
static const rp_msi_wcolumn_t binary_cols[] = { { "Name", KEY_S(72) }, { "Data", 0x0900u } };
static const rp_msi_wcolumn_t icon_cols[] = { { "Name", KEY_S(72) }, { "Data", 0x0900u } };
static const rp_msi_wcolumn_t condition_cols[] = { { "Feature_", KEY_S(38) }, { "Level", KEY_I2 }, { "Condition", S_N(255) } };
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
    char         **ini_short;   // short names of ir->inis' files, same order
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
    const uint8_t      *data;       // mapped (RFC-0013 E1)
    size_t              size;
    rp_map_t           *map;
    uint8_t             md5[16];    // of the bytes as first read; checked again at the end
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

// The Directory key of a dir ID: a [dir.*] that is a known folder itself (path = "Fonts") has no
// row of its own and stands for the standard folder property.
static const char *dkey(const rp_ir_t *ir, const char *id) {
    for (size_t i = 0; id && i < ir->dir_count; ++i) {
        const rp_ir_dir_t *d = &ir->dirs[i];
        if (d->part_count == 0 && d->base && strcmp(d->id, id) == 0) {
            const char *std = standard_folder(d->base, ir->arch);
            return std ? std : id;
        }
    }
    return id;
}

// The Directory key a shortcut goes to: a known folder's standard property, or its dir.
static const char *shortcut_dir_key(const rp_ir_t *ir, const rp_ir_shortcut_t *sc) {
    const char *std = standard_folder(sc->dir, ir->arch);
    bool known = strcmp(sc->dir, "Programs") == 0 || strcmp(sc->dir, "Desktop") == 0 || strcmp(sc->dir, "StartMenu") == 0 ||
                 strcmp(sc->dir, "Startup") == 0;
    return known && std ? std : dkey(ir, sc->dir);
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

void rp_build_files_free(proven_allocator_t alloc, rp_build_file_t *files, size_t count) {
    for (size_t i = 0; files && i < count; ++i) {
        rp_mem_free(alloc, files[i].name);
        rp_mem_free(alloc, files[i].data);
    }
    rp_mem_free(alloc, files);
}

// A license shown as text gets the Korean face when it has Hangul (the face also shows Latin).
static bool has_hangul(const uint8_t *t, size_t n) {
    for (size_t i = 0; i + 2 < n; ++i) {
        if (t[i] == 0xEA && t[i + 1] >= 0xB0) return true;      // U+AC00..U+AFFF
        if (t[i] >= 0xEB && t[i] <= 0xEC) return true;          // U+B000..U+CFFF
        if (t[i] == 0xED && t[i + 1] <= 0x9E) return true;      // U+D000..U+D7BF
        if (t[i] == 0xE1 && t[i + 1] >= 0x84 && t[i + 1] <= 0x87) return true;   // U+1100..U+11FF jamo
    }
    return false;
}

static bool ends_with_rtf(const char *s) {
    size_t n = strlen(s);
    return n > 4 && s[n - 4] == '.' && (s[n - 3] | 32) == 'r' && (s[n - 2] | 32) == 't' && (s[n - 1] | 32) == 'f';
}

// An .ico loaded into the Icon table (RFC-0013 A1).
typedef struct {
    const char *source, *name;
    uint8_t    *data;
} icon_t;

static const char *icon_name(proven_allocator_t alloc, keep_t *k, rows_t *icon, icon_t *icons, size_t *n, const char *source,
                             proven_err_t *err) {
    if (source == NULL || *err != PROVEN_OK) return NULL;
    for (size_t i = 0; i < *n; ++i) {
        if (strcmp(icons[i].source, source) == 0) return icons[i].name;
    }
    uint8_t *data = NULL;
    size_t len = 0;
    *err = rp_pal_read_file(alloc, source, 1u << 22, &data, &len);
    if (*err != PROVEN_OK) return NULL;
    char num[24];
    snprintf(num, sizeof num, "%zu", *n + 1);
    const char *name = kprintf(k, "RpIcon%s.ico", num, NULL);
    icons[(*n)++] = (icon_t){ source, name, data };
    s_(icon, name); b_(icon, data, len);
    return name;
}

// The tables of one package, and what their parts share while write_package fills them in order.
typedef struct {
    proven_allocator_t alloc;
    const rp_ir_t     *ir;
    keep_t            *k;
    lfile_t           *files;
    size_t             nfiles;
    dirs_t            *dirs;
    rp_srcdiags_t     *diags;
    rows_t property, directory, component, feature, featurecomp, file, filehash, media, upgrade, customaction, iexec, iui,
        aexec, aui, advt, createfolder, registry, removereg, shortcut, removefile, duplicate, environment, inifile, removeini, launch,
        appsearch, reglocator, drlocator, signature, complocator, svcinstall, svccontrol, font, lockperm, binary, icon, condition;
    icon_t          *icons;            // the Icon table's .ico files, in order of first use
    size_t           nicons;
    proven_err_t     icon_err;
    int32_t          comp_attr;        // a file's or folder's component: 64-bit unless x86
    bool             any_write, any_remove, any_qword, reg_bad, reglocator_dir;
    const char      *qplan;            // RP_QWORDS
    size_t          *group_end, ngroups;   // cabinet g holds files [group_end[g - 1], group_end[g])
    rp_msi_wstream_t *streams;         // embedded cabinets
    rp_build_file_t *ext;              // or external ones
    size_t           nstreams;
    rp_ui_t         *ui;
    uint8_t         *lic, *rtf, *banner, *lang_rtf[RP_UI_LANG_MAX];
} pkg_t;

// Property: identity, scope, [arp], [property.*] and the lists of secure and hidden properties.
static void lower_properties(pkg_t *pk, const char *product_code) {
    proven_allocator_t alloc = pk->alloc;
    const rp_ir_t *ir = pk->ir;
    keep_t *k = pk->k;
    char version3[24];
    snprintf(version3, sizeof version3, "%u.%u.%u", ir->version_parts[0], ir->version_parts[1], ir->version_parts[2]);
    char lang[8];
    snprintf(lang, sizeof lang, "%u", ir->language);
    s_(&pk->property, "ProductCode"); s_(&pk->property, product_code);
    s_(&pk->property, "ProductName"); s_(&pk->property, ir->name);
    s_(&pk->property, "ProductVersion"); s_(&pk->property, ir->version);
    s_(&pk->property, "Manufacturer"); s_(&pk->property, ir->manufacturer);
    s_(&pk->property, "ProductLanguage"); s_(&pk->property, kdup(k, lang));
    s_(&pk->property, "UpgradeCode"); s_(&pk->property, ir->upgrade_code);
    // Scope (RFC-0004 H3): machine ALLUSERS=1; user and dual use the single-package form ALLUSERS=2
    // with MSIINSTALLPERUSER=1 (per-user by default; dual installs per machine with ALLUSERS=1
    // MSIINSTALLPERUSER="" on the command line).
    if (ir->scope == 0) { s_(&pk->property, "ALLUSERS"); s_(&pk->property, "1"); }
    else { s_(&pk->property, "ALLUSERS"); s_(&pk->property, "2"); s_(&pk->property, "MSIINSTALLPERUSER"); s_(&pk->property, "1"); }
    if (ir->reboot_suppress) { s_(&pk->property, "REBOOT"); s_(&pk->property, "ReallySuppress"); }
    // Never close the user's programs to free a file (RFC-0003 X4): at /qn Restart Manager shuts
    // down every process holding a file, and fails the installation if one does not close. It is
    // authored here, not passed on the command line, so the old package's removal inside an
    // upgrade obeys it too (observed).
    s_(&pk->property, "MSIRESTARTMANAGERCONTROL"); s_(&pk->property, "Disable");
    // [arp] and [property.*] (RFC-0003 1). Secure and hidden properties are listed for the engine.
    // Icons (RFC-0013 A1): every .ico once in the Icon table (stream Icon.<Name>), named in order of
    // first use: the installed apps list first, then the shortcuts in ID order.
    pk->icons = rp_mem_alloc(alloc, ir->shortcut_count + 1, sizeof *pk->icons);
    if (pk->icons == NULL) pk->icon_err = PROVEN_ERR_NOMEM;
    const char *arp_icon = pk->icons ? icon_name(alloc, k, &pk->icon, pk->icons, &pk->nicons, ir->arp_icon_source, &pk->icon_err) : NULL;
    if (arp_icon) { s_(&pk->property, "ARPPRODUCTICON"); s_(&pk->property, arp_icon); }
    if (ir->arp_no_modify) { s_(&pk->property, "ARPNOMODIFY"); s_(&pk->property, "1"); }
    if (ir->arp_no_repair) { s_(&pk->property, "ARPNOREPAIR"); s_(&pk->property, "1"); }
    if (ir->arp_help) { s_(&pk->property, "ARPHELPLINK"); s_(&pk->property, ir->arp_help); }
    if (ir->arp_about) { s_(&pk->property, "ARPURLINFOABOUT"); s_(&pk->property, ir->arp_about); }
    char *secure = kdup(k, ir->refuse_below ? "RP_NEWER_FOUND;RP_OLDER_FOUND;RP_REFUSED_OLD" : "RP_NEWER_FOUND;RP_OLDER_FOUND");
    char *hidden = NULL;
    for (size_t i = 0; i < ir->property_count; ++i) {
        const rp_ir_property_t *p = &ir->properties[i];
        s_(&pk->property, p->id);
        s_(&pk->property, p->value);
        if (p->secure) secure = kprintf(k, "%s;%s", secure, p->id);
        if (p->hidden) hidden = hidden ? kprintf(k, "%s;%s", hidden, p->id) : kdup(k, p->id);
    }
    for (size_t i = 0; i < ir->search_count; ++i) secure = kprintf(k, "%s;%s", secure, ir->searches[i].property);
    // What an author's dialog collects runs the elevated part too (RFC-0005 K4).
    for (size_t i = 0; i < ir->dialog_control_count; ++i) {
        const char *pn = ir->dialog_controls[i].property;
        bool listed = false;
        for (size_t j = 0; pn && j < ir->property_count; ++j) listed |= ir->properties[j].secure && strcmp(ir->properties[j].id, pn) == 0;
        if (pn && !listed) secure = kprintf(k, "%s;%s", secure, pn);
    }
    // The language chosen in the dialogs reaches the elevated part (the guard's message, RFC-0012).
    if (ir->ui_lang_count > 1) secure = kprintf(k, "%s;%s", secure, "RPLANGUAGE");
    s_(&pk->property, "SecureCustomProperties"); s_(&pk->property, secure);
    if (hidden) { s_(&pk->property, "MsiHiddenProperties"); s_(&pk->property, hidden); }
}

// Directory: the folder tree with short|long names.
static void lower_directories(pkg_t *pk) {
    keep_t *k = pk->k;
    dirs_t *dirs = pk->dirs;
    for (size_t i = 0; i < dirs->count; ++i) {
        dnode_t *n = &dirs->v[i];
        s_(&pk->directory, n->key);
        s_(&pk->directory, n->parent);
        if (n->parent == NULL) s_(&pk->directory, "SourceDir");
        else if (n->long_name == NULL) s_(&pk->directory, ".");
        else if (strcmp(n->short_name, n->long_name) == 0) s_(&pk->directory, n->long_name);
        else s_(&pk->directory, kprintf(k, "%s|%s", n->short_name, n->long_name));
    }
}

// Features (Display: 0 hidden, then odd numbers in ID order)
static void lower_features(pkg_t *pk) {
    const rp_ir_t *ir = pk->ir;
    keep_t *k = pk->k;
    int display = 1;
    for (size_t i = 0; i < ir->feature_count; ++i) {
        const rp_ir_feature_t *f = &ir->features[i];
        s_(&pk->feature, f->id);
        s_(&pk->feature, f->parent);
        s_(&pk->feature, f->title);
        s_(&pk->feature, f->description);
        i_(&pk->feature, f->hidden ? 0 : display);
        if (!f->hidden) display += 2;
        i_(&pk->feature, f->level);
        null_(&pk->feature);
        i_(&pk->feature, (f->required ? 0x10 : 0) | (f->follow_parent ? 0x2 : 0));    // UIDisallowAbsent, FollowParent (RFC-0013 A3)
        if (f->when) {                  // off (level 0) unless its condition holds (RFC-0013 A2)
            // Only at the first installation: at removal the property is gone, and a feature turned
            // off then would keep its files (observed, r1-vm).
            s_(&pk->condition, f->id); i_(&pk->condition, 0); s_(&pk->condition, kprintf(k, "NOT Installed AND NOT (%s)", f->when, NULL));
        }
    }
}

// Files, components, hashes (in File key order = sequence order)
static void lower_files(pkg_t *pk) {
    const rp_ir_t *ir = pk->ir;
    keep_t *k = pk->k;
    lfile_t *files = pk->files;
    size_t nfiles = pk->nfiles;
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
        s_(&pk->component, lf->comp);
        s_(&pk->component, kdup(k, guid));
        s_(&pk->component, lf->dir_key);
        i_(&pk->component, pk->comp_attr | (lf->f->keep ? 16 : 0));     // keep: Permanent, left at removal (RFC-0013 A7)
        s_(&pk->component, lf->f->when);                            // RFC-0013 A2
        s_(&pk->component, lf->key);

        s_(&pk->featurecomp, lf->f->feature);
        s_(&pk->featurecomp, lf->comp);

        s_(&pk->file, lf->key);
        s_(&pk->file, lf->comp);
        if (strcmp(lf->short_name, lf->f->name) == 0) s_(&pk->file, lf->f->name);
        else s_(&pk->file, kprintf(k, "%s|%s", lf->short_name, lf->f->name));
        i_(&pk->file, (int32_t)lf->size);
        // Versioned files carry their version and language; the others get a hash row.
        rp_pe_info_t pi;
        bool versioned = rp_pe_read(lf->data, lf->size, &pi) == PROVEN_OK && pi.has_version;
        if (versioned) {
            char vtext[32], ltext[8];
            snprintf(vtext, sizeof vtext, "%u.%u.%u.%u", pi.version[0], pi.version[1], pi.version[2], pi.version[3]);
            snprintf(ltext, sizeof ltext, "%u", pi.language);
            s_(&pk->file, kdup(k, vtext));
            s_(&pk->file, kdup(k, ltext));
        } else {
            null_(&pk->file);
            null_(&pk->file);
        }
        i_(&pk->file, lf->f->vital ? 512 : 0);
        i_(&pk->file, (int32_t)(i + 1));
        if (versioned) continue;

        uint8_t d[16];
        memcpy(d, lf->md5, sizeof lf->md5);
        s_(&pk->filehash, lf->key);
        i_(&pk->filehash, 0);
        for (int p = 0; p < 4; ++p) {
            uint32_t w = (uint32_t)d[4 * p] | ((uint32_t)d[4 * p + 1] << 8) | ((uint32_t)d[4 * p + 2] << 16) |
                         ((uint32_t)d[4 * p + 3] << 24);
            i_(&pk->filehash, (int32_t)w);
        }
    }
}

// [registry.*] (RFC-0004): own component per value (key path = the value) unless `with`.
static void lower_registry(pkg_t *pk) {
    const rp_ir_t *ir = pk->ir;
    keep_t *k = pk->k;
    lfile_t *files = pk->files;
    size_t nfiles = pk->nfiles;
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
            static const char *const roots[] = { "HKMU", "HKCR", "HKCU", "HKLM" };  // index root + 1
            const char *logical = kprintf(k, "%s\\%s", roots[r->root + 1], r->key);
            logical = kprintf(k, "%s\\%s", logical, r->name ? r->name : "");
            const char *fields[] = { ir->upgrade_code, "machine", r->view32 ? "x86" : arch_text(ir->arch), logical, "registry", r->id };
            rp_uuid_derive("rubrapack.component", fields, 6, guid);
            ckey = kdup(k, comp);
            bool keypath = !r->remove && r->type != RP_REG_QWORD;     // a qword has no Registry row
            int32_t attr = (keypath ? 4 : 0) | (ir->arch != RP_ARCH_X86 && !r->view32 ? 256 : 0) | (r->keep ? 16 : 0);
            s_(&pk->component, ckey);
            s_(&pk->component, kdup(k, guid));
            s_(&pk->component, "TARGETDIR");
            i_(&pk->component, attr);
            s_(&pk->component, r->when);
            if (!keypath) null_(&pk->component);
            else s_(&pk->component, r->id);
            s_(&pk->featurecomp, r->feature);
            s_(&pk->featurecomp, ckey);
        }
        if (ckey == NULL) {
            pk->reg_bad = true;                     // the IR checked `with`; unreachable
            break;
        }
        const char *key = escape_formatted(k, r->key);
        const char *name = r->name ? escape_formatted(k, r->name) : NULL;
        if (r->remove) {
            s_(&pk->removereg, r->id); i_(&pk->removereg, (int32_t)r->root); s_(&pk->removereg, key);
            s_(&pk->removereg, name ? name : "-");  // no name: the whole key
            s_(&pk->removereg, ckey);
            pk->any_remove = true;
            continue;
        }
        if (r->type == RP_REG_QWORD) {      // RFC-0001 9.6: the helper DLL writes it (RP_QWORDS plan)
            static const char *const proots[] = { "HKMU", "HKCR", "HKCU", "HKLM" };
            const char *fields[] = { proots[r->root + 1], r->view32 || ir->arch == RP_ARCH_X86 ? "32" : "64", r->key,
                                     r->name ? r->name : "", r->value, ckey, r->keep ? "1" : "0" };
            for (size_t f = 0; f < sizeof fields / sizeof fields[0]; ++f) {
                rp_text_result_t u = rp_utf8_to_utf16((const uint8_t *)fields[f], strlen(fields[f]), NULL, 0);
                char head[24];
                snprintf(head, sizeof head, "%zu:", u.units);
                pk->qplan = kprintf(k, "%s%s", pk->qplan, head);
                pk->qplan = kprintf(k, "%s%s", pk->qplan, fields[f]);
            }
            pk->any_qword = true;
            continue;
        }
        const char *value = NULL;
        switch (r->type) {
        case RP_REG_QWORD: break;
        case RP_REG_STRING: value = r->value[0] == '#' ? kprintf(k, "#%s", r->value, NULL) : r->value; break;
        case RP_REG_EXPAND: value = kprintf(k, "#%%%s", r->value, NULL); break;
        case RP_REG_DWORD: value = kprintf(k, "#%s", r->value, NULL); break;
        case RP_REG_BINARY: value = kprintf(k, "#x%s", r->value, NULL); break;
        case RP_REG_MULTI:
            value = "[~]";
            for (size_t j = 0; j < r->item_count; ++j) value = kprintf(k, "%s%s[~]", value, r->items[j]);
            break;
        }
        s_(&pk->registry, r->id); i_(&pk->registry, (int32_t)r->root); s_(&pk->registry, key); s_(&pk->registry, name);
        s_(&pk->registry, value); s_(&pk->registry, ckey);
        pk->any_write = true;
    }
}

// [shortcut.*] (RFC-0004): in the target file's component; folders made for them are removed
// again at uninstall (RemoveFile, mode 2), from the shortcut's folder up to the known folder.
static void lower_shortcuts(pkg_t *pk) {
    proven_allocator_t alloc = pk->alloc;
    const rp_ir_t *ir = pk->ir;
    keep_t *k = pk->k;
    lfile_t *files = pk->files;
    size_t nfiles = pk->nfiles;
    dirs_t *dirs = pk->dirs;
    for (size_t i = 0; i < ir->shortcut_count; ++i) {
        const rp_ir_shortcut_t *sc = &ir->shortcuts[i];
        const lfile_t *target = NULL;
        for (size_t j = 0; j < nfiles; ++j) {
            if (strcmp(files[j].key, sc->target_file) == 0) target = &files[j];
        }
        if (target == NULL) {
            pk->reg_bad = true;
            break;
        }
        const char *sdir = shortcut_dir_key(ir, sc);
        const char *shortn = dirs->sc_short[i];
        s_(&pk->shortcut, sc->id);
        s_(&pk->shortcut, sdir);
        s_(&pk->shortcut, strcmp(shortn, sc->name) == 0 ? sc->name : kprintf(k, "%s|%s", shortn, sc->name));
        // Every shortcut has a component of its own (RFC-0013 A2 for `when`, RFC-0016 1 for all):
        // Windows' rules (ICE43, ICE57) treat the Start menu and the desktop as per-user places,
        // even in a per-machine package, and want such a component keyed by an HKCU value, not by
        // a program file. It is in the target file's feature, and it names the target by its folder
        // and name rather than [#File], which would tie it to the file's component (ICE69).
        const char *sc_comp = target->comp;
        bool own = true;
        if (own) {
            char comp[23], guid[39], rk[23];
            rp_key_derive('C', kprintf(k, "shortcut:%s", sc->id, NULL), comp);
            rp_key_derive('R', kprintf(k, "shortcut-when:%s", sc->id, NULL), rk);
            const char *fields[] = { ir->upgrade_code, "machine", arch_text(ir->arch), sc->id, "shortcut", sc->id };
            rp_uuid_derive("rubrapack.component", fields, 6, guid);
            sc_comp = kdup(k, comp);
            s_(&pk->component, sc_comp); s_(&pk->component, kdup(k, guid)); s_(&pk->component, sdir);
            i_(&pk->component, 4 | (ir->arch != RP_ARCH_X86 ? 256 : 0));
            if (sc->when) s_(&pk->component, sc->when); else null_(&pk->component);
            s_(&pk->component, kdup(k, rk));
            s_(&pk->featurecomp, target->f->feature); s_(&pk->featurecomp, sc_comp);
            s_(&pk->registry, kdup(k, rk)); i_(&pk->registry, 1); s_(&pk->registry, "Software\\[Manufacturer]\\[ProductName]\\Shortcuts");
            s_(&pk->registry, sc->id); s_(&pk->registry, "1"); s_(&pk->registry, sc_comp);
            pk->any_write = true;
        }
        s_(&pk->shortcut, sc_comp);
        s_(&pk->shortcut, kprintf(k, "[%s]%s", target->dir_key, escape_formatted(k, target->f->name)));
        s_(&pk->shortcut, sc->args);                                            // formatted (H2)
        s_(&pk->shortcut, sc->description);                                     // Text, not formatted
        const char *sc_icon = pk->icons ? icon_name(alloc, k, &pk->icon, pk->icons, &pk->nicons, sc->icon_source, &pk->icon_err) : NULL;
        null_(&pk->shortcut);                                                   // Hotkey
        if (sc_icon) { s_(&pk->shortcut, sc_icon); i_(&pk->shortcut, 0); } else { null_(&pk->shortcut); null_(&pk->shortcut); }
        null_(&pk->shortcut);                                                   // ShowCmd
        s_(&pk->shortcut, dkey(ir, sc->working_dir));
        for (const dnode_t *n = find_node(dirs, sdir); n && n->long_name && n->parent; n = find_node(dirs, n->parent)) {
            char rk[23];
            rp_key_derive('R', kprintf(k, "shortcut-folder:%s", n->key, NULL), rk);
            bool seen = false;
            for (size_t r = 0; r + 5 <= pk->removefile.filled; r += 5) {
                const rp_msi_cell_t *c0 = &pk->removefile.cells[r];
                seen |= c0->len == strlen(rk) && memcmp(c0->bytes, rk, c0->len) == 0;
            }
            if (seen) continue;
            s_(&pk->removefile, kdup(k, rk)); s_(&pk->removefile, own && !sc->when ? sc_comp : target->comp); null_(&pk->removefile);
            s_(&pk->removefile, n->key); i_(&pk->removefile, 2);
        }
    }
}

// [remove.*] (RFC-0004): own component in that folder (no key path file), one RemoveFile row.
static void lower_removes(pkg_t *pk) {
    const rp_ir_t *ir = pk->ir;
    keep_t *k = pk->k;
    for (size_t i = 0; i < ir->remove_count; ++i) {
        const rp_ir_remove_t *r = &ir->removes[i];
        char comp[23], guid[39];
        rp_key_derive('C', kprintf(k, "remove:%s", r->id, NULL), comp);
        const char *logical = kprintf(k, "%s/%s", r->dir, r->name ? r->name : "");
        const char *fields[] = { ir->upgrade_code, "machine", arch_text(ir->arch), logical, "remove", r->id };
        rp_uuid_derive("rubrapack.component", fields, 6, guid);
        const char *ckey = kdup(k, comp);
        s_(&pk->component, ckey); s_(&pk->component, kdup(k, guid)); s_(&pk->component, dkey(ir, r->dir));
        i_(&pk->component, ir->arch != RP_ARCH_X86 ? 256 : 0); null_(&pk->component); null_(&pk->component);
        s_(&pk->featurecomp, r->feature); s_(&pk->featurecomp, ckey);
        s_(&pk->createfolder, dkey(ir, r->dir)); s_(&pk->createfolder, ckey);     // its key path is the folder (ICE18)
        s_(&pk->removefile, r->id); s_(&pk->removefile, ckey); s_(&pk->removefile, r->name); s_(&pk->removefile, dkey(ir, r->dir));
        i_(&pk->removefile, r->mode);
    }
}

// [env.*] (RFC-0004): system variables ('*'), each its own component under TARGETDIR. With '-'
// the engine undoes it at uninstall: a set variable is deleted, appended or prepended text is
// taken out and the rest kept; without '-' (keep) nothing is undone (observed).
static void lower_env(pkg_t *pk) {
    const rp_ir_t *ir = pk->ir;
    keep_t *k = pk->k;
    for (size_t i = 0; i < ir->env_count; ++i) {
        const rp_ir_env_t *e = &ir->envs[i];
        char comp[23], guid[39];
        rp_key_derive('C', kprintf(k, "env:%s", e->id, NULL), comp);
        const char *fields[] = { ir->upgrade_code, "machine", arch_text(ir->arch), e->name, "env", e->id };
        rp_uuid_derive("rubrapack.component", fields, 6, guid);
        const char *ckey = kdup(k, comp);
        s_(&pk->component, ckey); s_(&pk->component, kdup(k, guid)); s_(&pk->component, "TARGETDIR");
        i_(&pk->component, (ir->arch != RP_ARCH_X86 ? 256 : 0) | (e->keep ? 16 : 0)); s_(&pk->component, e->when); null_(&pk->component);
        s_(&pk->featurecomp, e->feature); s_(&pk->featurecomp, ckey);
        const char *prefix = ir->scope == 1 ? (!e->keep ? "=-" : "=") : (!e->keep ? "=-*" : "=*");  // no '*': user variable
        const char *value = e->mode == 1 ? kprintf(k, "[~];%s", e->value, NULL)
                          : e->mode == 2 ? kprintf(k, "%s;[~]", e->value, NULL)
                                         : e->value;
        s_(&pk->environment, e->id); s_(&pk->environment, kprintf(k, "%s%s", prefix, e->name));
        s_(&pk->environment, value); s_(&pk->environment, ckey);
    }
}

// [ini.*] (RFC-0004): one component per entry in the INI file's folder. IniFile rows are undone
// at uninstall by the engine; mode remove is a RemoveIniFile row applied at install.
static void lower_ini(pkg_t *pk) {
    const rp_ir_t *ir = pk->ir;
    keep_t *k = pk->k;
    dirs_t *dirs = pk->dirs;
    for (size_t i = 0; i < ir->ini_count; ++i) {
        const rp_ir_ini_t *x = &ir->inis[i];
        char comp[23], guid[39];
        rp_key_derive('C', kprintf(k, "ini:%s", x->id, NULL), comp);
        const char *logical = kprintf(k, "%s/%s", x->dir, x->file);
        logical = kprintf(k, "%s[%s]", logical, x->section);
        logical = kprintf(k, "%s%s", logical, x->key);
        const char *fields[] = { ir->upgrade_code, "machine", arch_text(ir->arch), logical, "ini", x->id };
        rp_uuid_derive("rubrapack.component", fields, 6, guid);
        const char *ckey = kdup(k, comp);
        s_(&pk->component, ckey); s_(&pk->component, kdup(k, guid)); s_(&pk->component, dkey(ir, x->dir));
        i_(&pk->component, ir->arch != RP_ARCH_X86 ? 256 : 0); s_(&pk->component, x->when); null_(&pk->component);
        s_(&pk->featurecomp, x->feature); s_(&pk->featurecomp, ckey);
        s_(&pk->createfolder, dkey(ir, x->dir)); s_(&pk->createfolder, ckey);     // its key path is the folder (ICE18)
        const char *shortn = dirs->ini_short[i];
        const char *fname = shortn && strcmp(shortn, x->file) != 0 ? kprintf(k, "%s|%s", shortn, x->file) : x->file;
        rows_t *t = x->mode == 2 ? &pk->removeini : &pk->inifile;
        s_(t, x->id); s_(t, fname); s_(t, dkey(ir, x->dir));
        s_(t, escape_formatted(k, x->section)); s_(t, escape_formatted(k, x->key));
        s_(t, x->value);                                            // formatted (H2); NULL for remove
        i_(t, x->mode == 0 ? 0 : x->mode == 1 ? 3 : 2);             // addLine, addTag, removeLine
        s_(t, ckey);
    }
}

// [service.*] (RFC-0004): in the exe's component. Stopped before files change at install and at
// removal, deleted at removal, optionally started after installation; waits for each step.
static void lower_services(pkg_t *pk) {
    const rp_ir_t *ir = pk->ir;
    keep_t *k = pk->k;
    lfile_t *files = pk->files;
    size_t nfiles = pk->nfiles;
    for (size_t i = 0; i < ir->service_count; ++i) {
        const rp_ir_service_t *x = &ir->services[i];
        const lfile_t *exe = NULL;
        for (size_t j = 0; j < nfiles; ++j) {
            if (strcmp(files[j].key, x->file) == 0) exe = &files[j];
        }
        if (exe == NULL) {
            pk->reg_bad = true;
            break;
        }
        static const char *const accounts[] = { NULL, "NT AUTHORITY\\LocalService", "NT AUTHORITY\\NetworkService" };
        const char *name = escape_formatted(k, x->name);
        s_(&pk->svcinstall, x->id); s_(&pk->svcinstall, name);
        s_(&pk->svcinstall, x->display_name ? escape_formatted(k, x->display_name) : NULL);
        i_(&pk->svcinstall, 0x10); i_(&pk->svcinstall, x->start); i_(&pk->svcinstall, 1);    // own process, normal errors
        null_(&pk->svcinstall); null_(&pk->svcinstall);
        s_(&pk->svcinstall, accounts[x->account]); null_(&pk->svcinstall);
        s_(&pk->svcinstall, x->args); s_(&pk->svcinstall, exe->comp);
        s_(&pk->svcinstall, x->description ? escape_formatted(k, x->description) : NULL);   // formatted (observed)
        int event = 0x2 | 0x20 | 0x80 | (x->start_on_install ? 0x1 : 0);         // stop (both), delete (remove), start
        s_(&pk->svccontrol, x->id); s_(&pk->svccontrol, name); i_(&pk->svccontrol, event); null_(&pk->svccontrol);
        i_(&pk->svccontrol, 1); s_(&pk->svccontrol, exe->comp);
    }
}

// The helper DLL for qword values (RFC-0001 9.6): Binary "RpCa" with the part for this
// architecture, the plan in RP_QWORDS, and prepare (immediate) -> rollback twin -> apply.
// RFC-0012 V6: the dirs with guard = true, as Directory keys.
static void lower_helper_actions(pkg_t *pk) {
    const rp_ir_t *ir = pk->ir;
    keep_t *k = pk->k;
    rp_srcdiags_t *diags = pk->diags;
    const char *guard = NULL;
    for (size_t i = 0; i < ir->dir_count; ++i) {
        if (ir->dirs[i].guard) guard = guard ? kprintf(k, "%s;%s", guard, dkey(ir, ir->dirs[i].id)) : dkey(ir, ir->dirs[i].id);
    }
    if (pk->any_qword || guard) {
        const unsigned char *part = ir->arch == RP_ARCH_X64 ? rp_ca_x64 : ir->arch == RP_ARCH_X86 ? rp_ca_x86 : rp_ca_arm64;
        size_t part_len = ir->arch == RP_ARCH_X64 ? rp_ca_x64_len : ir->arch == RP_ARCH_X86 ? rp_ca_x86_len : rp_ca_arm64_len;
        if (part_len == 0) {
            rp_srcdiag_add(diags, (rp_pos_t){ 1, 1 }, "RP1901", false,
                           "%s needs resources/bin/rubrapack_ca-%s.dll, which this rubrapack was built without",
                           pk->any_qword ? "type = \"qword\"" : "guard = true", arch_text(ir->arch));
            pk->reg_bad = true;
        }
        s_(&pk->binary, "RpCa"); b_(&pk->binary, part, part_len);
    }
    // RFC-0013 A5: the finished page's program, started by the setup's own (non-elevated) client,
    // without waiting: type 34 (an exe in a folder) + 0xC0 (asynchronous, no wait).
    if (ir->ui_launch_file && ir->ui != RP_UI_NONE) {
        const rp_ir_file_t *lf = NULL;
        for (size_t i = 0; i < ir->file_count; ++i) {
            if (strcmp(ir->files[i].id, ir->ui_launch_file) == 0) lf = &ir->files[i];
        }
        if (lf) {
            s_(&pk->customaction, "RP_Launch"); i_(&pk->customaction, 34 | 0xC0); s_(&pk->customaction, dkey(ir, lf->dir));
            s_(&pk->customaction, ir->ui_launch_args ? kprintf(k, "\"[#%s]\" %s", lf->id, ir->ui_launch_args) : kprintf(k, "\"[#%s]\"", lf->id, NULL));
        }
    }
    // The guard (immediate, first installation only, before any file is placed): its message in
    // each language of the dialogs, picked by RPLANGUAGE (English without dialogs).
    if (guard) {
        s_(&pk->property, "RP_GUARD"); s_(&pk->property, guard);
        size_t nl = ir->ui_lang_count ? ir->ui_lang_count : 1;
        for (size_t li = 0; li < nl; ++li) {
            s_(&pk->property, kprintf(k, "RpGuardMsg_%s", ir->ui_lang_count ? ir->ui_langs[li].code : "en", NULL));
            s_(&pk->property, rp_ui_text_for(ir, "DirGuardText", li));
        }
        s_(&pk->customaction, "RP_GuardDirs"); i_(&pk->customaction, 1); s_(&pk->customaction, "RpCa"); s_(&pk->customaction, "RpGuardDirs");
        s_(&pk->iexec, "RP_GuardDirs"); s_(&pk->iexec, "NOT Installed"); i_(&pk->iexec, 1010);
    }
    if (pk->any_qword) {
        s_(&pk->property, "RP_QWORDS"); s_(&pk->property, pk->qplan);
        const int noimp = ir->scope == 0 ? 0x800 : 0;
        s_(&pk->customaction, "RP_QwordPrepare"); i_(&pk->customaction, 1); s_(&pk->customaction, "RpCa"); s_(&pk->customaction, "RpQwordPrepare");
        s_(&pk->customaction, "RP_QwordApplyRollback"); i_(&pk->customaction, 1 | 0x100 | 0x400 | noimp | 0x40);
        s_(&pk->customaction, "RpCa"); s_(&pk->customaction, "RpQwordRollback");
        s_(&pk->customaction, "RP_QwordApply"); i_(&pk->customaction, 1 | 0x400 | noimp); s_(&pk->customaction, "RpCa");
        s_(&pk->customaction, "RpQwordApply");
    }
}

// [permission.*] (RFC-0004): MsiLockPermissionsEx on a File, a Registry row, or a CreateFolder row
// (a folder gets its own component that creates it). Applied by InstallFiles, WriteRegistryValues,
// CreateFolders.
static void lower_permissions(pkg_t *pk) {
    const rp_ir_t *ir = pk->ir;
    keep_t *k = pk->k;
    for (size_t i = 0; i < ir->permission_count; ++i) {
        const rp_ir_permission_t *x = &ir->permissions[i];
        static const char *const tables[] = { "CreateFolder", "File", "Registry" };
        const char *lock = x->target;
        if (x->kind == 0) {
            char comp[23], guid[39];
            rp_key_derive('C', kprintf(k, "permission:%s", x->id, NULL), comp);
            const char *fields[] = { ir->upgrade_code, "machine", arch_text(ir->arch), x->target, "permission", x->id };
            rp_uuid_derive("rubrapack.component", fields, 6, guid);
            const char *ckey = kdup(k, comp);
            lock = dkey(ir, x->target);
            s_(&pk->component, ckey); s_(&pk->component, kdup(k, guid)); s_(&pk->component, lock);
            i_(&pk->component, ir->arch != RP_ARCH_X86 ? 256 : 0); null_(&pk->component); null_(&pk->component);
            s_(&pk->featurecomp, x->feature); s_(&pk->featurecomp, ckey);
            s_(&pk->createfolder, lock); s_(&pk->createfolder, ckey);
        }
        s_(&pk->lockperm, x->id); s_(&pk->lockperm, lock); s_(&pk->lockperm, tables[x->kind]); s_(&pk->lockperm, x->sddl); null_(&pk->lockperm);
    }
}

// [font.*] (RFC-0004): the file is already in FontsFolder; the Font row registers it.
static void lower_fonts(pkg_t *pk) {
    const rp_ir_t *ir = pk->ir;
    for (size_t i = 0; i < ir->font_count; ++i) {
        s_(&pk->font, ir->fonts[i].file); s_(&pk->font, ir->fonts[i].title);
    }
}

// [require.*] and [search.*] (RFC-0004).
// Launch conditions only guard a first installation: repair and removal must never be blocked
// (the engine even deletes MSIINSTALLPERUSER once a per-user product is installed - observed).
static void lower_searches(pkg_t *pk) {
    const rp_ir_t *ir = pk->ir;
    keep_t *k = pk->k;
    for (size_t i = 0; i < ir->require_count; ++i) {
        s_(&pk->launch, kprintf(k, "Installed OR (%s)", ir->requires[i].condition, NULL)); s_(&pk->launch, ir->requires[i].message);
    }
    if (ir->scope == 1) {           // a per-user package stays per user
        s_(&pk->launch, "Installed OR MSIINSTALLPERUSER = 1");
        s_(&pk->launch, ir->language == 1042 ? "[ProductName]은(는) 사용자별로만 설치합니다(MSIINSTALLPERUSER=1)."
                                         : "[ProductName] installs for the current user only (MSIINSTALLPERUSER=1).");
    }
    for (size_t i = 0; i < ir->search_count; ++i) {
        const rp_ir_search_t *x = &ir->searches[i];
        // A search that fills a dir (RFC-0012 V5) lands in RpFound_<ID>; a type-51 action right
        // after AppSearch copies it to the dir only when the dir is still unset, so a value given on
        // the command line wins.
        const char *found = x->fills_dir ? kprintf(k, "RpFound_%s", x->id, NULL) : x->property;
        s_(&pk->appsearch, found); s_(&pk->appsearch, x->id);
        if (x->fills_dir) {
            const char *act = kprintf(k, "RpFill_%s", x->id, NULL);
            s_(&pk->customaction, act); i_(&pk->customaction, 51); s_(&pk->customaction, x->property); s_(&pk->customaction, kprintf(k, "[%s]", found, NULL));
            const char *cnd = kprintf(k, "%s AND NOT %s", found, x->property);
            s_(&pk->iexec, act); s_(&pk->iexec, cnd); i_(&pk->iexec, 51);
            s_(&pk->iui, act); s_(&pk->iui, cnd); i_(&pk->iui, 51);
        }
        switch (x->kind) {
        case RP_SEARCH_REGISTRY:    // type 2 = the raw value, 0 = a folder that must exist; +16 = the 64-bit view
            s_(&pk->reglocator, x->id); i_(&pk->reglocator, (int32_t)x->root); s_(&pk->reglocator, escape_formatted(k, x->key));
            s_(&pk->reglocator, x->name ? escape_formatted(k, x->name) : NULL);
            i_(&pk->reglocator, (x->fills_dir ? 0 : 2) | (x->view32 ? 0 : 16));
            pk->reglocator_dir |= x->fills_dir;
            break;
        case RP_SEARCH_FILE:
        case RP_SEARCH_DIR: {
            const char *std = standard_folder(x->base, ir->arch);
            const char *path = x->path ? kprintf(k, "[%s]%s", std, x->path) : kprintf(k, "[%s]", std, NULL);
            s_(&pk->drlocator, x->id); null_(&pk->drlocator); s_(&pk->drlocator, path); i_(&pk->drlocator, 0);
            if (x->kind == RP_SEARCH_FILE) {
                s_(&pk->signature, x->id); s_(&pk->signature, x->file_name); s_(&pk->signature, x->min_version);
                for (int n = 0; n < 6; ++n) null_(&pk->signature);
            }
            break;
        }
        case RP_SEARCH_COMPONENT:   // type 1 = the full path of the component's key file
            s_(&pk->complocator, x->id); s_(&pk->complocator, x->component_guid); i_(&pk->complocator, 1);
            break;
        }
    }
}

// [copy.*] (RFC-0004): DuplicateFile in the source file's component.
static void lower_copies(pkg_t *pk) {
    const rp_ir_t *ir = pk->ir;
    keep_t *k = pk->k;
    lfile_t *files = pk->files;
    size_t nfiles = pk->nfiles;
    dirs_t *dirs = pk->dirs;
    for (size_t i = 0; i < ir->copy_count; ++i) {
        const rp_ir_copy_t *cp = &ir->copies[i];
        const lfile_t *src = NULL;
        for (size_t j = 0; j < nfiles; ++j) {
            if (strcmp(files[j].key, cp->source_file) == 0) src = &files[j];
        }
        if (src == NULL) {
            pk->reg_bad = true;
            break;
        }
        const char *longn = cp->name ? cp->name : src->f->name;
        const char *shortn = dirs->cp_short[i];
        s_(&pk->duplicate, cp->id); s_(&pk->duplicate, src->comp); s_(&pk->duplicate, src->key);
        s_(&pk->duplicate, strcmp(shortn, longn) == 0 ? longn : kprintf(k, "%s|%s", shortn, longn));
        s_(&pk->duplicate, dkey(ir, cp->dir));
    }
}

// Folders: CreateFolder, one component each, the folder itself as the key path.
static void lower_folders(pkg_t *pk) {
    const rp_ir_t *ir = pk->ir;
    keep_t *k = pk->k;
    for (size_t i = 0; i < ir->folder_count; ++i) {
        const rp_ir_folder_t *f = &ir->folders[i];
        char comp[23], guid[39];
        rp_key_derive('C', f->id, comp);
        const char *logical = kprintf(k, "%s/%s", f->dir, f->name);
        const char *fields[] = { ir->upgrade_code, "machine", arch_text(ir->arch), logical, "folder", f->id };
        rp_uuid_derive("rubrapack.component", fields, 6, guid);
        const char *ckey = kdup(k, comp);
        s_(&pk->component, ckey);
        s_(&pk->component, kdup(k, guid));
        s_(&pk->component, f->id);
        i_(&pk->component, pk->comp_attr | (f->keep ? 16 : 0));
        null_(&pk->component);
        null_(&pk->component);
        s_(&pk->featurecomp, f->feature);
        s_(&pk->featurecomp, ckey);
        s_(&pk->createfolder, f->id);
        s_(&pk->createfolder, ckey);
    }
}

// Media and the cabinets: files in sequence order, a new cabinet when the next file would pass
// cab-max-size (a file larger than that gets a cabinet of its own). One Media row per cabinet.
static proven_err_t lower_cabinets(pkg_t *pk, const char *cab_stem, const rp_limits_t *limits, size_t jobs) {
    proven_allocator_t alloc = pk->alloc;
    const rp_ir_t *ir = pk->ir;
    keep_t *k = pk->k;
    lfile_t *files = pk->files;
    size_t nfiles = pk->nfiles;
    rp_srcdiags_t *diags = pk->diags;
    proven_err_t err = pk->reg_bad ? PROVEN_ERR_INVALID_ARG : pk->icon_err;
    pk->group_end = rp_mem_alloc(alloc, nfiles + 1, sizeof *pk->group_end);
    pk->streams = rp_mem_alloc(alloc, nfiles + 1, sizeof *pk->streams);
    pk->ext = ir->cab_external ? rp_mem_alloc(alloc, nfiles + 1, sizeof *pk->ext) : NULL;
    if (pk->group_end == NULL || pk->streams == NULL || (ir->cab_external && pk->ext == NULL)) err = PROVEN_ERR_NOMEM;
    // External cabinets without cab-max-size are split before 2 GiB each (RFC-0013 E4).
    uint64_t cab_max = ir->cab_max ? ir->cab_max : ir->cab_external ? (2ull << 30) - (64ull << 20) : 0;
    for (size_t i = 0, used = 0; err == PROVEN_OK && i < nfiles; ++i) {
        if (cab_max && used > 0 && used + files[i].size > cab_max) {
            pk->group_end[pk->ngroups++] = i;
            used = 0;
        }
        used += files[i].size;
    }
    if (err == PROVEN_OK && nfiles) pk->group_end[pk->ngroups++] = nfiles;
    if (nfiles == 0) {
        i_(&pk->media, 1); i_(&pk->media, 0); null_(&pk->media); null_(&pk->media); null_(&pk->media); null_(&pk->media);
    }
    uint64_t embedded = 0;
    for (size_t g = 0, start = 0; err == PROVEN_OK && g < pk->ngroups; start = pk->group_end[g++]) {
        char num[24];
        snprintf(num, sizeof num, "%zu", g + 1);
        const char *name = !ir->cab_external ? kprintf(k, "cab%s.cab", num, NULL)
                           : pk->ngroups == 1    ? kprintf(k, "%s.cab", cab_stem, NULL)
                                             : kprintf(k, "%s-%s.cab", cab_stem, num);
        i_(&pk->media, (int32_t)(g + 1)); i_(&pk->media, (int32_t)pk->group_end[g]); null_(&pk->media);
        s_(&pk->media, ir->cab_external ? name : kprintf(k, "#%s", name, NULL)); null_(&pk->media); null_(&pk->media);
        size_t n = pk->group_end[g] - start;
        rp_cab_file_t *cf = rp_mem_alloc(alloc, n, sizeof *cf);
        uint8_t *cab = NULL;
        size_t cab_len = 0;
        if (cf == NULL) {
            err = PROVEN_ERR_NOMEM;
            break;
        }
        for (size_t i = 0; i < n; ++i) cf[i] = (rp_cab_file_t){ files[start + i].key, files[start + i].data, files[start + i].size };
        err = rp_cab_write_ex(alloc, cf, n, ir->compress, jobs ? jobs : rp_pal_cpu_count(), limits, &cab, &cab_len);
        rp_mem_free(alloc, cf);
        if (err != PROVEN_OK) break;
        if (ir->cab_external) {
            size_t nl = strlen(name) + 1;
            char *copy = rp_mem_alloc(alloc, nl, 1);
            if (copy == NULL) {
                rp_mem_free(alloc, cab);
                err = PROVEN_ERR_NOMEM;
                break;
            }
            memcpy(copy, name, nl);
            pk->ext[pk->nstreams++] = (rp_build_file_t){ copy, cab, cab_len };
        } else {
            pk->streams[pk->nstreams++] = (rp_msi_wstream_t){ name, cab, cab_len };
            embedded += cab_len;
        }
    }
    // Windows Installer does not open a package of 2 GiB or more (RFC-0013 E4).
    if (err == PROVEN_OK && embedded >= (2ull << 30) - (16ull << 20)) {
        rp_srcdiag_add(diags, (rp_pos_t){ 1, 1 }, "RP1516", false,
                       "the embedded cabinets make the package 2 GiB or more, which Windows Installer cannot open; use cab = \"external\"");
        err = PROVEN_ERR_OUT_OF_BOUNDS;
    }
    return err;
}

// Upgrade: detect same-or-newer (refused below), remove older (RFC-0001 9.5)
static void lower_upgrade(pkg_t *pk) {
    const rp_ir_t *ir = pk->ir;
    keep_t *k = pk->k;
    s_(&pk->upgrade, ir->upgrade_code); s_(&pk->upgrade, ir->version); null_(&pk->upgrade); null_(&pk->upgrade);
    i_(&pk->upgrade, 0x002 | 0x100); null_(&pk->upgrade); s_(&pk->upgrade, "RP_NEWER_FOUND");
    s_(&pk->upgrade, ir->upgrade_code); null_(&pk->upgrade); s_(&pk->upgrade, ir->version); null_(&pk->upgrade);
    i_(&pk->upgrade, 0x001); null_(&pk->upgrade); s_(&pk->upgrade, "RP_OLDER_FOUND");
    if (ir->refuse_below) {         // detect only (0x002), below the given version (max exclusive)
        s_(&pk->upgrade, ir->upgrade_code); null_(&pk->upgrade); s_(&pk->upgrade, ir->refuse_below); null_(&pk->upgrade);
        i_(&pk->upgrade, 0x002); null_(&pk->upgrade); s_(&pk->upgrade, "RP_REFUSED_OLD");
    }

    const char *message = ir->downgrade_message ? escape_formatted(k, ir->downgrade_message)
                          : ir->language == 1042 ? "더 새 판이나 같은 판의 [ProductName]이(가) 이미 설치되어 있습니다."
                                                 : "The same or a newer version of [ProductName] is already installed.";
    s_(&pk->customaction, "RP_RefuseDowngrade"); i_(&pk->customaction, 19); null_(&pk->customaction); s_(&pk->customaction, message);
    // RFC-0003 section 9 (T1): an old version that must be removed by hand first. The removal command
    // names the found product ([RP_REFUSED_OLD]) and keeps Restart Manager off for that removal.
    if (ir->refuse_below) {
        const char *cmd = "msiexec /x [RP_REFUSED_OLD] /qn MSIRESTARTMANAGERCONTROL=Disable";
        const char *text = ir->refuse_message ? escape_formatted(k, ir->refuse_message)
                           : ir->language == 1042 ? "설치된 옛 판 [ProductName]은(는) 이 설치로 올릴 수 없습니다. 먼저 지운 뒤 다시 설치하십시오:"
                                                  : "The installed older version of [ProductName] cannot be upgraded by this package. Remove it first, then run this installation again:";
        s_(&pk->customaction, "RP_RefuseOld"); i_(&pk->customaction, 19); null_(&pk->customaction);
        s_(&pk->customaction, kprintf(k, "%s %s", text, cmd));
    }
}

// The install, UI, administrative and advertisement sequences, with the [action.*] do/undo pairs.
static proven_err_t lower_sequences(pkg_t *pk) {
    const rp_ir_t *ir = pk->ir;
    keep_t *k = pk->k;
    lfile_t *files = pk->files;
    size_t nfiles = pk->nfiles;
    rp_srcdiags_t *diags = pk->diags;
    proven_err_t err = PROVEN_OK;
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
        s_(&pk->iexec, exec[i].action); s_(&pk->iexec, exec[i].cond); i_(&pk->iexec, exec[i].seq);
    }
    if (ir->font_count) {           // MS Learn "Suggested InstallExecuteSequence"
        s_(&pk->iexec, "UnregisterFonts"); null_(&pk->iexec); i_(&pk->iexec, 2500);
        s_(&pk->iexec, "RegisterFonts"); null_(&pk->iexec); i_(&pk->iexec, 5300);
    }
    if (ir->service_count) {        // MS Learn "Suggested InstallExecuteSequence"
        s_(&pk->iexec, "StopServices"); null_(&pk->iexec); i_(&pk->iexec, 1900);
        s_(&pk->iexec, "DeleteServices"); null_(&pk->iexec); i_(&pk->iexec, 2000);
        s_(&pk->iexec, "InstallServices"); null_(&pk->iexec); i_(&pk->iexec, 5800);
        s_(&pk->iexec, "StartServices"); null_(&pk->iexec); i_(&pk->iexec, 5900);
    }
    if (ir->search_count) {         // before the launch conditions, which may test the results
        s_(&pk->iexec, "AppSearch"); null_(&pk->iexec); i_(&pk->iexec, 50);
        s_(&pk->iui, "AppSearch"); null_(&pk->iui); i_(&pk->iui, 50);
    }
    if (ir->require_count || ir->scope == 1) {
        s_(&pk->iexec, "LaunchConditions"); null_(&pk->iexec); i_(&pk->iexec, 100);
        s_(&pk->iui, "LaunchConditions"); null_(&pk->iui); i_(&pk->iui, 100);
    }
    if (ir->ini_count) {            // MS Learn "Suggested InstallExecuteSequence"
        s_(&pk->iexec, "RemoveIniValues"); null_(&pk->iexec); i_(&pk->iexec, 3320);
        s_(&pk->iexec, "WriteIniValues"); null_(&pk->iexec); i_(&pk->iexec, 5100);
    }
    if (ir->env_count) {            // MS Learn "Suggested InstallExecuteSequence"
        s_(&pk->iexec, "RemoveEnvironmentStrings"); null_(&pk->iexec); i_(&pk->iexec, 3310);
        s_(&pk->iexec, "WriteEnvironmentStrings"); null_(&pk->iexec); i_(&pk->iexec, 5200);
    }
    if (ir->copy_count) {           // MS Learn "Suggested InstallExecuteSequence" (3400 is used by our Undo pairs)
        s_(&pk->iexec, "RemoveDuplicateFiles"); null_(&pk->iexec); i_(&pk->iexec, 3300);
        s_(&pk->iexec, "DuplicateFiles"); null_(&pk->iexec); i_(&pk->iexec, 4210);
    }
    if (ir->shortcut_count) {       // MS Learn "Suggested InstallExecuteSequence"
        s_(&pk->iexec, "RemoveShortcuts"); null_(&pk->iexec); i_(&pk->iexec, 3200);
        s_(&pk->iexec, "CreateShortcuts"); null_(&pk->iexec); i_(&pk->iexec, 4500);
    }
    if (pk->any_qword) {                // after WriteRegistryValues; the prepare step reads component states
        s_(&pk->iexec, "RP_QwordPrepare"); null_(&pk->iexec); i_(&pk->iexec, 5010);
        s_(&pk->iexec, "RP_QwordApplyRollback"); null_(&pk->iexec); i_(&pk->iexec, 5011);
        s_(&pk->iexec, "RP_QwordApply"); null_(&pk->iexec); i_(&pk->iexec, 5012);
    }
    if (pk->any_write || pk->any_remove) {  // MS Learn "Suggested InstallExecuteSequence"
        s_(&pk->iexec, "RemoveRegistryValues"); null_(&pk->iexec); i_(&pk->iexec, 2600);
        s_(&pk->iexec, "WriteRegistryValues"); null_(&pk->iexec); i_(&pk->iexec, 5000);
    }
    if (ir->refuse_below) {
        s_(&pk->iexec, "RP_RefuseOld"); s_(&pk->iexec, "RP_REFUSED_OLD"); i_(&pk->iexec, 31);
        s_(&pk->iui, "RP_RefuseOld"); s_(&pk->iui, "RP_REFUSED_OLD"); i_(&pk->iui, 31);
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
        // Per machine the pair runs elevated (not impersonated); per user, as the user.
        const int noimp = ir->scope == 0 ? 0x800 : 0;
        const int FORWARD = 18 | 0x400 | noimp, ROLLBACK = 18 | 0x100 | 0x400 | noimp | 0x40;
        struct { const char *suffix; int type; const char *args; const char *cond; int seq; } rows[] = {
            { "UndoRollback", ROLLBACK, do_args, remove, 3400 + 2 * (int)(na - 1 - i) },
            { "Undo", FORWARD, undo_args, remove, 3401 + 2 * (int)(na - 1 - i) },
            { "DoRollback", ROLLBACK, undo_args, fresh, 4001 + 3 * (int)i },
            { "RedoRollback", ROLLBACK, do_args, again, 4002 + 3 * (int)i },
            { "Do", FORWARD, do_args, install, 4003 + 3 * (int)i },
        };
        for (size_t r = 0; r < sizeof rows / sizeof rows[0]; ++r) {
            const char *name = kprintf(k, "RP_%s_%s", a->id, rows[r].suffix);
            s_(&pk->customaction, name); i_(&pk->customaction, rows[r].type); s_(&pk->customaction, run->key);
            s_(&pk->customaction, rows[r].args);
            s_(&pk->iexec, name); s_(&pk->iexec, rows[r].cond); i_(&pk->iexec, rows[r].seq);
        }
    }
    static const struct { const char *action; const char *cond; int seq; } ui_rows[] = {
        { "FindRelatedProducts", NULL, 25 }, { "RP_RefuseDowngrade", "RP_NEWER_FOUND", 30 },
        { "CostInitialize", NULL, 800 }, { "FileCost", NULL, 900 }, { "CostFinalize", NULL, 1000 },
        { "MigrateFeatureStates", NULL, 1200 }, { "ExecuteAction", NULL, 1300 },
    };
    for (size_t i = 0; i < sizeof ui_rows / sizeof ui_rows[0]; ++i) {
        s_(&pk->iui, ui_rows[i].action); s_(&pk->iui, ui_rows[i].cond); i_(&pk->iui, ui_rows[i].seq);
    }
    // Administrative installation (msiexec /a: an uncompressed network image) and advertisement
    // (msiexec /jm), from MS Learn "Suggested AdminExecuteSequence / AdvtExecuteSequence" (RFC-0001
    // 9.1, P3). None of the package's own actions run there.
    static const struct { const char *action; int seq; } admin_exec[] = {
        { "CostInitialize", 800 }, { "FileCost", 900 }, { "CostFinalize", 1000 }, { "InstallValidate", 1400 },
        { "InstallInitialize", 1500 }, { "InstallAdminPackage", 3900 }, { "InstallFiles", 4000 }, { "InstallFinalize", 6600 },
    };
    for (size_t i = 0; i < sizeof admin_exec / sizeof admin_exec[0]; ++i) {
        s_(&pk->aexec, admin_exec[i].action); null_(&pk->aexec); i_(&pk->aexec, admin_exec[i].seq);
    }
    static const struct { const char *action; int seq; } admin_ui[] = {
        { "CostInitialize", 800 }, { "FileCost", 900 }, { "CostFinalize", 1000 }, { "ExecuteAction", 1300 },
    };
    for (size_t i = 0; i < sizeof admin_ui / sizeof admin_ui[0]; ++i) {
        s_(&pk->aui, admin_ui[i].action); null_(&pk->aui); i_(&pk->aui, admin_ui[i].seq);
    }
    static const struct { const char *action; int seq; } advt_exec[] = {
        { "CostInitialize", 800 }, { "CostFinalize", 1000 }, { "InstallValidate", 1400 }, { "InstallInitialize", 1500 },
        { "PublishFeatures", 6300 }, { "PublishProduct", 6400 }, { "InstallFinalize", 6600 },
    };
    for (size_t i = 0; i < sizeof advt_exec / sizeof advt_exec[0]; ++i) {
        s_(&pk->advt, advt_exec[i].action); null_(&pk->advt); i_(&pk->advt, advt_exec[i].seq);
    }
    return err;
}

// Dialog sets (RFC-0005): the tables come from ui.c; its Property, InstallUISequence and Binary
// rows join ours.
static proven_err_t lower_dialogs(pkg_t *pk) {
    proven_allocator_t alloc = pk->alloc;
    const rp_ir_t *ir = pk->ir;
    size_t lic_len = 0, rtf_len = 0, banner_len = 0;
    size_t lang_rtf_len[RP_UI_LANG_MAX] = { 0 };
    if (ir->ui == RP_UI_NONE) return PROVEN_OK;
    proven_err_t err = PROVEN_OK;
    if (ir->license_source) {
        err = rp_pal_read_file(alloc, ir->license_source, 1u << 22, &pk->lic, &lic_len);
        if (err == PROVEN_OK) {
            size_t n = strlen(ir->license_source);
            bool is_rtf = n > 4 && (ir->license_source[n - 1] | 32) == 'f' && (ir->license_source[n - 2] | 32) == 't' &&
                          (ir->license_source[n - 3] | 32) == 'r' && ir->license_source[n - 4] == '.';
            if (is_rtf) {
                pk->rtf = pk->lic;
                rtf_len = lic_len;
                pk->lic = NULL;
            } else {
                err = rp_ui_text_to_rtf(alloc, pk->lic, lic_len, has_hangul(pk->lic, lic_len), &pk->rtf, &rtf_len);
            }
        }
    }
    // RFC-0012: [ui] license-xx, each converted as the common one is.
    for (size_t li = 0; err == PROVEN_OK && li < ir->ui_lang_count && li < RP_UI_LANG_MAX; ++li) {
        const char *srcf = ir->ui_langs[li].license_source;
        if (srcf == NULL) continue;
        uint8_t *raw = NULL;
        size_t raw_len = 0;
        err = rp_pal_read_file(alloc, srcf, 1u << 22, &raw, &raw_len);
        if (err != PROVEN_OK) break;
        if (ends_with_rtf(srcf)) {
            pk->lang_rtf[li] = raw;
            lang_rtf_len[li] = raw_len;
        } else {
            err = rp_ui_text_to_rtf(alloc, raw, raw_len, has_hangul(raw, raw_len), &pk->lang_rtf[li], &lang_rtf_len[li]);
            rp_mem_free(alloc, raw);
        }
    }
    if (err == PROVEN_OK && ir->banner_source) err = rp_pal_read_file(alloc, ir->banner_source, 1u << 22, &pk->banner, &banner_len);
    if (err == PROVEN_OK) {
        rp_ui_input_t in = { dkey(ir, ir->ui_install_dir ? ir->ui_install_dir : "INSTALLDIR"), pk->rtf, rtf_len, pk->banner, banner_len,
                             { 0 }, { 0 } };
        for (size_t li = 0; li < RP_UI_LANG_MAX; ++li) {
            in.license_rtf_by_lang[li] = pk->lang_rtf[li];
            in.license_len_by_lang[li] = lang_rtf_len[li];
        }
        err = rp_ui_build(alloc, ir, &in, &pk->ui);
        if (err == PROVEN_ERR_INVALID_STATE)    // ui.c's own check of its tab orders: our bug, not the input's
            rp_diag_error(RP_DIAG_INTERNAL, "internal error: a built-in dialog has a broken tab order; please report it");
    }
    for (size_t i = 0; pk->ui && i < pk->ui->prop_count; ++i) {
        if (pk->ui->props[i].value) { s_(&pk->property, pk->ui->props[i].name); s_(&pk->property, pk->ui->props[i].value); }
    }
    for (size_t i = 0; pk->ui && i < pk->ui->seq_count; ++i) {
        s_(&pk->iui, pk->ui->seqs[i].action); s_(&pk->iui, pk->ui->seqs[i].condition); i_(&pk->iui, pk->ui->seqs[i].sequence);
        // The finished, cancelled and failed pages end an administrative installation too (ICE20).
        if (pk->ui->seqs[i].sequence < 0) {
            s_(&pk->aui, pk->ui->seqs[i].action); s_(&pk->aui, pk->ui->seqs[i].condition); i_(&pk->aui, pk->ui->seqs[i].sequence);
        }
    }
    for (size_t i = 0; pk->ui && i < pk->ui->ca_count; ++i) {       // RFC-0012: set-property actions
        s_(&pk->customaction, pk->ui->cas[i].action); i_(&pk->customaction, 51); s_(&pk->customaction, pk->ui->cas[i].source);
        s_(&pk->customaction, pk->ui->cas[i].target);
    }
    for (size_t t = 0; pk->ui && t < pk->ui->table_count; ++t) {      // ui's Binary rows into ours
        const rp_msi_wtable_t *wt = &pk->ui->tables[t];
        if (strcmp(wt->name, "Binary") != 0) continue;
        for (size_t r = 0; r < wt->row_count; ++r) {
            const rp_msi_cell_t *c0 = &wt->cells[r * 2], *c1 = &wt->cells[r * 2 + 1];
            s_(&pk->binary, (const char *)c0->bytes);          // a NUL-terminated literal in ui.c
            b_(&pk->binary, c1->bytes, c1->len);
        }
    }
    return err;
}

static proven_err_t write_package(proven_allocator_t alloc, const rp_ir_t *ir, keep_t *k, lfile_t *files, size_t nfiles,
                                  dirs_t *dirs, const char *product_code, const char *package_code,
                                  const rp_limits_t *limits, uint8_t **out, size_t *len, rp_srcdiags_t *diags,
                                  const char *cab_stem, rp_build_file_t **xcabs, size_t *nxcabs, size_t jobs,
                                  const rp_out_sink_t *sink) {
    pkg_t pkg = { .alloc = alloc, .ir = ir, .k = k, .files = files, .nfiles = nfiles, .dirs = dirs, .diags = diags,
                  .comp_attr = ir->arch == RP_ARCH_X86 ? 0 : 256, .qplan = "RPQ1" };
    pkg_t *pk = &pkg;
    rows_init(&pk->property, alloc, "Property", property_cols, 2);
    rows_init(&pk->directory, alloc, "Directory", directory_cols, 3);
    rows_init(&pk->component, alloc, "Component", component_cols, 6);
    rows_init(&pk->feature, alloc, "Feature", feature_cols, 8);
    rows_init(&pk->featurecomp, alloc, "FeatureComponents", featurecomp_cols, 2);
    rows_init(&pk->file, alloc, "File", file_cols, 8);
    rows_init(&pk->filehash, alloc, "MsiFileHash", filehash_cols, 6);
    rows_init(&pk->media, alloc, "Media", media_cols, 6);
    rows_init(&pk->upgrade, alloc, "Upgrade", upgrade_cols, 7);
    rows_init(&pk->customaction, alloc, "CustomAction", customaction_cols, 4);
    rows_init(&pk->iexec, alloc, "InstallExecuteSequence", sequence_cols, 3);
    rows_init(&pk->iui, alloc, "InstallUISequence", sequence_cols, 3);
    rows_init(&pk->aexec, alloc, "AdminExecuteSequence", sequence_cols, 3);
    rows_init(&pk->aui, alloc, "AdminUISequence", sequence_cols, 3);
    rows_init(&pk->advt, alloc, "AdvtExecuteSequence", sequence_cols, 3);
    rows_init(&pk->createfolder, alloc, "CreateFolder", createfolder_cols, 2);
    rows_init(&pk->registry, alloc, "Registry", registry_cols, 6);
    rows_init(&pk->removereg, alloc, "RemoveRegistry", removereg_cols, 5);
    rows_init(&pk->shortcut, alloc, "Shortcut", shortcut_cols, 12);
    rows_init(&pk->removefile, alloc, "RemoveFile", removefile_cols, 5);
    rows_init(&pk->duplicate, alloc, "DuplicateFile", duplicate_cols, 5);
    rows_init(&pk->environment, alloc, "Environment", environment_cols, 4);
    rows_init(&pk->inifile, alloc, "IniFile", inifile_cols, 8);
    rows_init(&pk->removeini, alloc, "RemoveIniFile", removeini_cols, 8);
    rows_init(&pk->launch, alloc, "LaunchCondition", launch_cols, 2);
    rows_init(&pk->appsearch, alloc, "AppSearch", appsearch_cols, 2);
    rows_init(&pk->reglocator, alloc, "RegLocator", reglocator_cols, 5);
    rows_init(&pk->drlocator, alloc, "DrLocator", drlocator_cols, 4);
    rows_init(&pk->signature, alloc, "Signature", signature_cols, 9);
    rows_init(&pk->complocator, alloc, "CompLocator", complocator_cols, 3);
    rows_init(&pk->svcinstall, alloc, "ServiceInstall", svcinstall_cols, 13);
    rows_init(&pk->svccontrol, alloc, "ServiceControl", svccontrol_cols, 6);
    rows_init(&pk->font, alloc, "Font", font_cols, 2);
    rows_init(&pk->lockperm, alloc, "MsiLockPermissionsEx", lockperm_cols, 5);
    rows_init(&pk->binary, alloc, "Binary", binary_cols, 2);
    rows_init(&pk->icon, alloc, "Icon", icon_cols, 2);
    rows_init(&pk->condition, alloc, "Condition", condition_cols, 3);
    // The P3 tables are written only when they have rows, so packages without them stay as they were.
    rows_t *all[] = { &pk->property, &pk->directory, &pk->component, &pk->feature, &pk->featurecomp, &pk->file,
                      &pk->filehash, &pk->media, &pk->upgrade, &pk->customaction, &pk->iexec, &pk->iui, &pk->createfolder,
                      &pk->aexec, &pk->aui, &pk->advt, &pk->registry, &pk->removereg, &pk->shortcut, &pk->removefile,
                      &pk->duplicate, &pk->environment, &pk->inifile, &pk->removeini, &pk->launch, &pk->appsearch,
                      &pk->reglocator, &pk->drlocator, &pk->signature, &pk->complocator, &pk->svcinstall, &pk->svccontrol,
                      &pk->font, &pk->lockperm, &pk->binary, &pk->icon, &pk->condition };
    const size_t always = 16;

    lower_properties(pk, product_code);
    lower_directories(pk);
    lower_features(pk);
    lower_files(pk);
    lower_registry(pk);
    lower_shortcuts(pk);
    lower_removes(pk);
    lower_env(pk);
    lower_ini(pk);
    lower_services(pk);
    lower_helper_actions(pk);
    lower_permissions(pk);
    lower_fonts(pk);
    lower_searches(pk);
    lower_copies(pk);
    lower_folders(pk);
    proven_err_t err = lower_cabinets(pk, cab_stem, limits, jobs);
    lower_upgrade(pk);
    proven_err_t seq_err = lower_sequences(pk);
    if (seq_err != PROVEN_OK) err = seq_err;
    if (err == PROVEN_OK) err = lower_dialogs(pk);

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
        { 7, tmpl, 0 }, { 9, package_code, 0 }, { 14, NULL, ir->arch == RP_ARCH_ARM64 || ir->permission_count ? 500 : 200 },
        { 15, NULL, ir->scope ? 2 | 8 : 2 }, { 18, app, 0 },
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
        rp_msi_wtable_t tables[sizeof all / sizeof all[0] + 9];   // + the ui tables + _Validation
        size_t nt = 0;
        for (size_t i = 0; i < sizeof all / sizeof all[0]; ++i) {
            // AppSearch looks a folder-type RegLocator up in Signature, which must then exist even
            // when empty (error 2228 otherwise: RFC-0012 V5 on the VM).
            bool need = all[i] == &pk->signature && pk->reglocator_dir;
            if (i < always || all[i]->t.row_count > 0 || need) tables[nt++] = all[i]->t;
        }
        for (size_t t = 0; pk->ui && t < pk->ui->table_count; ++t) {
            if (strcmp(pk->ui->tables[t].name, "Binary") != 0) tables[nt++] = pk->ui->tables[t];
        }
        const char *missing = NULL;
        err = rp_msi_validation(alloc, tables, nt, &tables[nt], &missing);
        if (err == PROVEN_ERR_NOT_FOUND) {
            rp_srcdiag_add(diags, (rp_pos_t){ 1, 1 }, "RP2020", false, "internal: no _Validation rule for a column of %s", missing);
        }
        if (err == PROVEN_OK) {
            rp_msi_wdb_t db = { 65001, tables, nt + 1, summary, summary_len, pk->streams, ir->cab_external ? 0 : pk->nstreams };
            err = rp_msi_lint(alloc, &db, diags);     // RFC-0001 7.1: build always checks what it writes
            if (err == PROVEN_OK) err = rp_msi_write_to(alloc, &db, 12, limits, sink, out, len);
            rp_mem_free(alloc, (void *)tables[nt].cells);
        }
    }
    for (size_t i = 0; i < sizeof all / sizeof all[0]; ++i) rp_mem_free(alloc, all[i]->cells);
    if (!ir->cab_external) {
        for (size_t i = 0; i < pk->nstreams; ++i) rp_mem_free(alloc, (void *)pk->streams[i].data);
    }
    rp_mem_free(alloc, pk->streams);
    rp_mem_free(alloc, pk->group_end);
    rp_ui_free(alloc, pk->ui);
    rp_mem_free(alloc, pk->lic);
    rp_mem_free(alloc, pk->rtf);
    rp_mem_free(alloc, pk->banner);
    for (size_t i = 0; pk->icons && i < pk->nicons; ++i) rp_mem_free(alloc, pk->icons[i].data);
    rp_mem_free(alloc, pk->icons);
    for (size_t li = 0; li < RP_UI_LANG_MAX; ++li) rp_mem_free(alloc, pk->lang_rtf[li]);
    rp_mem_free(alloc, summary);
    if (ir->cab_external) {
        if (err == PROVEN_OK) {
            *xcabs = pk->ext;
            *nxcabs = pk->nstreams;
        } else {
            rp_build_files_free(alloc, pk->ext, pk->nstreams);
        }
    }
    return err;
}

proven_err_t rp_msi_from_ir(proven_allocator_t alloc, const rp_ir_t *ir, const rp_build_options_t *opt,
                            const rp_limits_t *limits, uint8_t **out, size_t *len,
                            rp_build_file_t **cabs, size_t *cab_count, rp_srcdiags_t *diags) {
    if (ir == NULL || opt == NULL || limits == NULL || out == NULL || len == NULL || cabs == NULL || cab_count == NULL ||
        diags == NULL) {
        return PROVEN_ERR_INVALID_ARG;
    }
    *cabs = NULL;
    *cab_count = 0;
    bool done = false;              // the reproducible build's first pass was the package
    // Tests make several cabinet folders from a small package (RFC-0013 E4).
    char *fb = rp_pal_getenv(alloc, "RP_TEST_CAB_FOLDER_BLOCKS");
    if (fb) {
        long v = strtol(fb, NULL, 10);
        if (v > 0 && v <= 0xFFFF) rp_cab_folder_blocks = (size_t)v;
        rp_mem_free(alloc, fb);
    }
    const char *stem = opt->cab_stem ? opt->cab_stem : "cab";
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
        add_node(&dirs, kdup(&k, f->id), kdup(&k, dkey(ir, f->dir)), kprintf(&k, "%s/%s", parent_logical, f->name), kdup(&k, f->name));
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
        lf->dir_key = kdup(&k, dkey(ir, f->dir));
        size_t n = 0;
        if (rp_pal_map_file(alloc, f->source_path, limits->max_output, &lf->data, &n, &lf->map) != PROVEN_OK) {
            rp_srcdiag_add(diags, f->pos, "RP1509", false, "cannot read source file '%s'", f->source);
            err = PROVEN_ERR_IO;
            break;
        }
        lf->size = n;
        rp_md5(lf->data, n, lf->md5);
        // One snapshot (RFC-0001 14.2): the bytes read here are hashed, versioned and packed; the
        // model's earlier look (size, PE machine) must describe the same file.
        rp_pe_info_t snap;
        uint16_t machine = rp_pe_read(lf->data, n, &snap) == PROVEN_OK && snap.is_pe ? snap.machine : 0;
        if ((uint64_t)n != f->size || machine != f->pe_machine) {
            rp_srcdiag_add(diags, f->pos, "RP1515", false, "source file '%s' changed while the package was being built", f->source);
            err = PROVEN_ERR_IO;
            break;
        }
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
    dirs.ini_short = rp_mem_alloc(alloc, ir->ini_count + 1, sizeof *dirs.ini_short);
    if (dirs.sc_short == NULL || dirs.cp_short == NULL || dirs.ini_short == NULL) err = PROVEN_ERR_NOMEM;
    for (size_t i = 0; err == PROVEN_OK && i < ir->ini_count; ++i) dirs.ini_short[i] = NULL;
    for (size_t i = 0; err == PROVEN_OK && i < ir->copy_count; ++i) dirs.cp_short[i] = NULL;
    for (size_t i = 0; err == PROVEN_OK && i < ir->shortcut_count; ++i) {
        dirs.sc_short[i] = NULL;
        const char *key = shortcut_dir_key(ir, &ir->shortcuts[i]);
        if (find_node(&dirs, key) == NULL) add_node(&dirs, kdup(&k, key), "TARGETDIR", kdup(&k, key), NULL);
    }

    // Short names per folder: its sub folders, its files and its shortcuts share one namespace.
    for (size_t i = 0; err == PROVEN_OK && i < dirs.count; ++i) {
        const char *folder = dirs.v[i].key;
        size_t n = 0;
        for (size_t j = 0; j < dirs.count; ++j) n += dirs.v[j].parent && dirs.v[j].long_name && strcmp(dirs.v[j].parent, folder) == 0;
        for (size_t j = 0; j < ir->file_count; ++j) n += strcmp(files[j].dir_key, folder) == 0;
        for (size_t j = 0; j < ir->shortcut_count; ++j) n += strcmp(shortcut_dir_key(ir, &ir->shortcuts[j]), folder) == 0;
        for (size_t j = 0; j < ir->copy_count; ++j) n += strcmp(dkey(ir, ir->copies[j].dir), folder) == 0;
        for (size_t j = 0; j < ir->ini_count; ++j) n += strcmp(dkey(ir, ir->inis[j].dir), folder) == 0;
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
            if (strcmp(shortcut_dir_key(ir, &ir->shortcuts[j]), folder) == 0) {
                s[m++] = (sib_t){ ir->shortcuts[j].name, &dirs.sc_short[j] };
            }
        }
        for (size_t j = 0; j < ir->copy_count; ++j) {
            if (strcmp(dkey(ir, ir->copies[j].dir), folder) != 0) continue;
            const char *longn = ir->copies[j].name;
            for (size_t f = 0; longn == NULL && f < ir->file_count; ++f) {
                if (strcmp(ir->files[f].id, ir->copies[j].source_file) == 0) longn = ir->files[f].name;
            }
            s[m++] = (sib_t){ longn ? longn : ir->copies[j].id, &dirs.cp_short[j] };
        }
        // An INI file that is also installed here, or named by an earlier entry, shares that short name
        // (filled in below); only new INI files take part in the assignment.
        for (size_t j = 0; j < ir->ini_count; ++j) {
            const rp_ir_ini_t *x = &ir->inis[j];
            if (strcmp(dkey(ir, x->dir), folder) != 0) continue;
            bool shared = false;
            for (size_t f = 0; f < ir->file_count; ++f) shared |= strcmp(files[f].dir_key, folder) == 0 && strcmp(files[f].f->name, x->file) == 0;
            for (size_t e = 0; e < j; ++e) shared |= strcmp(dkey(ir, ir->inis[e].dir), folder) == 0 && strcmp(ir->inis[e].file, x->file) == 0;
            if (!shared) s[m++] = (sib_t){ x->file, &dirs.ini_short[j] };
        }
        assign_short(&k, s, m);
        rp_mem_free(alloc, s);
    }

    for (size_t j = 0; err == PROVEN_OK && j < ir->ini_count; ++j) {     // the shared INI short names
        const rp_ir_ini_t *x = &ir->inis[j];
        for (size_t f = 0; dirs.ini_short[j] == NULL && f < ir->file_count; ++f) {
            if (strcmp(files[f].dir_key, dkey(ir, x->dir)) == 0 && strcmp(files[f].f->name, x->file) == 0) dirs.ini_short[j] = files[f].short_name;
        }
        for (size_t e = 0; dirs.ini_short[j] == NULL && e < j; ++e) {
            if (strcmp(ir->inis[e].dir, x->dir) == 0 && strcmp(ir->inis[e].file, x->file) == 0) dirs.ini_short[j] = dirs.ini_short[e];
        }
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
            // The two passes differ only in that code (the summary stream), so it is put into the
            // first pass's bytes in place when it can be found there once; otherwise (it straddles
            // two sectors) the package is written again, with the same result.
            uint8_t *first = NULL;
            size_t first_len = 0;
            rp_build_file_t *fcabs = NULL;
            size_t nfcabs = 0;
            static const char zero[] = "{00000000-0000-0000-0000-000000000000}";
            err = write_package(alloc, ir, &k, files, ir->file_count, &dirs, product_code, zero, limits, &first, &first_len,
                                diags, stem, &fcabs, &nfcabs, opt->jobs, opt->sink);
            if (err == PROVEN_OK) {
                uint8_t d[PROVEN_SHA256_SIZE];
                proven_sha256((proven_mem_view_t){ first, first_len }, d);
                for (size_t i = 0; i < nfcabs; ++i) {
                    uint8_t both[2 * PROVEN_SHA256_SIZE];
                    memcpy(both, d, PROVEN_SHA256_SIZE);
                    proven_sha256((proven_mem_view_t){ fcabs[i].data, fcabs[i].len }, both + PROVEN_SHA256_SIZE);
                    proven_sha256((proven_mem_view_t){ both, sizeof both }, d);
                }
                char hex[65];
                for (int i = 0; i < 32; ++i) snprintf(hex + 2 * i, 3, "%02x", d[i]);
                const char *fields[] = { hex };
                rp_uuid_derive("rubrapack.package", fields, 1, package_code);
                size_t zl = sizeof zero - 1, hits = 0, at = 0;
                for (size_t i = 0; i + zl <= first_len; ++i) {
                    if (first[i] == '{' && memcmp(first + i, zero, zl) == 0) {
                        ++hits;
                        at = i;
                    }
                }
                char *two = rp_pal_getenv(alloc, "RP_TEST_TWO_PASS");     // tests: take the other road
                if (hits == 1 && two == NULL && strlen(package_code) == zl) {
                    memcpy(first + at, package_code, zl);
                    *out = first;
                    *len = first_len;
                    *cabs = fcabs;
                    *cab_count = nfcabs;
                    done = true;
                } else {
                    rp_build_files_free(alloc, fcabs, nfcabs);
                    if (opt->sink) opt->sink->drop(opt->sink->ctx, first);
                    else rp_mem_free(alloc, first);
                }
                rp_mem_free(alloc, two);
            }
        } else {
            err = rp_uuid_random(package_code);
        }
    }
    if (err == PROVEN_OK && !done) {
        err = write_package(alloc, ir, &k, files, ir->file_count, &dirs, product_code, package_code, limits, out, len,
                            opt->reproducible ? &(rp_srcdiags_t){ 0 } : diags,  // the first pass reported already
                            stem, cabs, cab_count, opt->jobs, opt->sink);
    }
    if (err == PROVEN_OK && ir->summary_name == NULL && !is_ascii(ir->name)) {
        rp_srcdiag_add(diags, (rp_pos_t){ 1, 1 }, "RP1203", true,
                       "name is not ASCII and summary-name is missing; the summary Subject will read 'rubrapack package'");
    }

    // One snapshot (RFC-0001 14.2) with mapped files: what was packed is still what was hashed.
    for (size_t i = 0; err == PROVEN_OK && files && i < ir->file_count; ++i) {
        uint8_t again[16];
        rp_md5(files[i].data, files[i].size, again);
        if (memcmp(again, files[i].md5, sizeof again) != 0) {
            rp_srcdiag_add(diags, files[i].f->pos, "RP1515", false, "source file '%s' changed while the package was being built",
                           files[i].f->source);
            err = PROVEN_ERR_IO;
        }
    }
    for (size_t i = 0; files && i < ir->file_count; ++i) rp_pal_unmap(alloc, files[i].map);
    rp_mem_free(alloc, files);
    rp_mem_free(alloc, final_logical);
    rp_mem_free(alloc, dirs.sc_short);
    rp_mem_free(alloc, dirs.cp_short);
    rp_mem_free(alloc, dirs.ini_short);
    rp_mem_free(alloc, dirs.v);
    keep_free(&k);
    if (err == PROVEN_OK && k.nomem) err = PROVEN_ERR_NOMEM;
    return err;
}
