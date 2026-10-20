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
#include "rubrapack/merge.h"
#include "rubrapack/msi.h"
#include "rubrapack/msix.h"
#include "rubrapack/num.h"
#include "rubrapack/pal.h"
#include "rubrapack/parts.h"
#include "rubrapack/pe.h"
#include "rubrapack/suminfo.h"
#include "rubrapack/text.h"
#include "rubrapack/lang.h"
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
#define KEY_L(n)  (0x2F00u | (n))

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
// RFC-0031 wave 1.
static const rp_msi_wcolumn_t movefile_cols[] = { { "FileKey", KEY_S(72) }, { "Component_", S(72) }, { "SourceName", L_N(255) },
                                                  { "DestName", L_N(255) }, { "SourceFolder", S_N(72) }, { "DestFolder", S(72) },
                                                  { "Options", I2 } };
static const rp_msi_wcolumn_t odbcdriver_cols[] = { { "Driver", KEY_S(72) }, { "Component_", S(72) }, { "Description", L(255) },
                                                    { "File_", S(72) }, { "File_Setup", S_N(72) } };
static const rp_msi_wcolumn_t odbcattr_cols[] = { { "Driver_", KEY_S(72) }, { "Attribute", KEY_L(40) }, { "Value", L_N(255) } };
static const rp_msi_wcolumn_t odbcsource_cols[] = { { "DataSource", KEY_S(72) }, { "Component_", S(72) }, { "Description", L(255) },
                                                    { "DriverDescription", L(255) }, { "Registration", I2 } };
static const rp_msi_wcolumn_t odbcsattr_cols[] = { { "DataSource_", KEY_S(72) }, { "Attribute", KEY_L(32) }, { "Value", L_N(255) } };
static const rp_msi_wcolumn_t publish_cols[] = { { "ComponentId", KEY_S(38) }, { "Qualifier", KEY_L(255) }, { "Component_", KEY_S(72) },
                                                 { "AppData", L_N(255) }, { "Feature_", S(38) } };
static const rp_msi_wcolumn_t isolated_cols[] = { { "Component_Shared", KEY_S(72) }, { "Component_Application", KEY_S(72) } };
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
        { "TARGETDIR", "TARGETDIR", "TARGETDIR" },      // a merge module's root
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

// The face of a license shown as text: the language's ([ui] font-xx or its built-in face) by a
// name RTF can carry, else the language's built-in face (the model warns, RP1319); a license common
// to every language gets the Korean face when it has Hangul (the face also shows Latin), else
// English's.
static const char *rtf_face(const char *face, const char *code) {
    const char *a = rp_ui_face_ascii(face);
    if (a) return a;
    static const char *const builtin[][2] = { { "ko", "Malgun Gothic" }, { "ja", "Yu Gothic UI" }, { "zh", "Microsoft YaHei UI" }, { "zh_TW", "Microsoft JhengHei UI" },
                                              { "th", "Leelawadee UI" } };
    for (size_t k = 0; k < sizeof builtin / sizeof builtin[0]; ++k) {
        if (strcmp(code, builtin[k][0]) == 0) return builtin[k][1];
    }
    return "Segoe UI";
}

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
        appsearch, reglocator, drlocator, signature, complocator, svcinstall, svccontrol, font, lockperm, binary, icon, condition,
        movefile, odbcdriver, odbcattr, odbcsource, odbcsattr, publish, isolated;
    icon_t          *icons;            // the Icon table's .ico files, in order of first use
    size_t           nicons;
    proven_err_t     icon_err;
    int32_t          comp_attr;        // a file's or folder's component: 64-bit unless x86
    bool             any_write, any_remove, any_qword, any_rplan, any_purge, any_others, cleanup, preflight, reg_bad, menu, maintain, details, report;
    bool             extra;            // the preflight looks for what was not installed in the product's folders (extra-files)
    const char      *qplan;            // RP_QWORDS
    const char      *rplan;            // RP_REMOVES ([remove] upgrade = false)
    const char      *pplan;            // RP_PURGE ([remove] recursive, [registry] on = "uninstall")
    const char      *newer_cond, *older_cond;   // RP_RefuseDowngrade's and RP_RefuseOld's conditions (refusal)
    size_t          *group_end, ngroups;   // cabinet g holds files [group_end[g - 1], group_end[g])
    rp_msi_wstream_t *streams;         // embedded cabinets
    rp_build_file_t *ext;              // or external ones
    size_t           nstreams;
    rp_ui_t         *ui;
    uint8_t         *lic, *rtf, *banner, *lang_rtf[RP_UI_LANG_MAX];
    rp_ui_control_image_t *ctl_imgs;    // B10: bitmap dialog-controls' bytes, alive until the package is written
    size_t           nctl_img;
    uint8_t         *act_bin[64];      // RFC-0031 B6: Binary-table DLLs of single actions, freed at the end
    size_t           act_bin_count;
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
    if (ir->arp_no_remove) { s_(&pk->property, "ARPNOREMOVE"); s_(&pk->property, "1"); }
    if (ir->arp_help) { s_(&pk->property, "ARPHELPLINK"); s_(&pk->property, ir->arp_help); }
    if (ir->arp_about) { s_(&pk->property, "ARPURLINFOABOUT"); s_(&pk->property, ir->arp_about); }
    char *secure = kdup(k, ir->refuse_below ? "RP_NEWER_FOUND;RP_OLDER_FOUND;RP_REFUSED_OLD" : "RP_NEWER_FOUND;RP_OLDER_FOUND");
    if (!ir->no_preflight && !ir->module) secure = kprintf(k, "%s;%s", secure, "RPCLOSE");     // the answer for a run without UI
    bool users_all = false;         // RFC-0030: the other users' too, for a removal without UI
    for (size_t i = 0; i < ir->remove_count; ++i) users_all |= ir->removes[i].users_all;
    for (size_t i = 0; i < ir->registry_count; ++i) users_all |= ir->registries[i].users_all;
    if (users_all) secure = kprintf(k, "%s;%s", secure, "RPPURGEUSERS");
    for (size_t i = 0; i < ir->replace_count; ++i) {      // x47: the replaced products' action properties
        char n[40];
        snprintf(n, sizeof n, "RP_REPLACED_%u", (unsigned)(i & 15));
        secure = kprintf(k, "%s;%s", secure, n);
    }
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
        i_(&pk->feature, (f->required ? 0x10 : 0) | (f->follow_parent ? 0x2 : 0) | (f->no_advertise ? 0x8 : 0));    // UIDisallowAbsent, FollowParent (RFC-0013 A3), DisallowAdvertise (B8)
        if (f->when) {                  // off (level 0) unless its condition holds (RFC-0013 A2)
            // Only at the first installation: at removal the property is gone, and a feature turned
            // off then would keep its files (observed, r1-vm).
            s_(&pk->condition, f->id); i_(&pk->condition, 0); s_(&pk->condition, kprintf(k, "NOT Installed AND NOT (%s)", f->when, NULL));
        }
        if (f->default_when) {          // x48: level 1 - installed by default - when its condition holds
            s_(&pk->condition, f->id); i_(&pk->condition, 1); s_(&pk->condition, kprintf(k, "NOT Installed AND (%s)", f->default_when, NULL));
        }
    }
}

// The dir `id` lies in the user's profile: it is one of the profile's known folders, or its base is.
static bool dir_in_profile(const rp_ir_t *ir, const char *id) {
    static const char *const profile[] = { "AppData", "LocalAppData", "StartMenu", "Programs", "Desktop", "Startup" };
    for (size_t depth = 0; id && depth <= ir->dir_count; ++depth) {
        const rp_ir_dir_t *d = NULL;
        for (size_t i = 0; i < ir->dir_count; ++i) {
            if (strcmp(ir->dirs[i].id, id) == 0) d = &ir->dirs[i];
        }
        const char *base = d ? d->base : id;
        if (d == NULL || d->base) {
            for (size_t i = 0; i < sizeof profile / sizeof profile[0]; ++i) {
                if (strcmp(base, profile[i]) == 0) return true;
            }
            return false;
        }
        id = d->parent;
    }
    return false;
}

// A component in the user's profile (RFC-0029): its key path is an HKCU value, as a shortcut's is
// (ICE38) - Software\[Manufacturer]\[ProductName]\Components, named after the component, value 1.
// Returns the Registry row's key; the caller puts it in KeyPath and 4 in Attributes.
static const char *profile_keypath(pkg_t *pk, const char *ckey) {
    keep_t *k = pk->k;
    char rk[23];
    rp_key_derive('R', kprintf(k, "component-keypath:%s", ckey, NULL), rk);
    const char *rkey = kdup(k, rk);
    s_(&pk->registry, rkey); i_(&pk->registry, 1); s_(&pk->registry, "Software\\[Manufacturer]\\[ProductName]\\Components");
    s_(&pk->registry, ckey); s_(&pk->registry, "1"); s_(&pk->registry, ckey);
    pk->any_write = true;
    return rkey;
}

// Whether component `ckey` is removed with the product: not kept (Permanent) and without a condition.
static bool comp_removed(const pkg_t *pk, const char *ckey) {
    const rows_t *t = &pk->component;
    for (size_t r = 0; r + 6 <= t->filled; r += 6) {
        const rp_msi_cell_t *c = &t->cells[r];
        if (c->len == strlen(ckey) && memcmp(c->bytes, ckey, c->len) == 0) return !(t->cells[r + 3].i & 16) && t->cells[r + 4].kind == RP_MSI_NULL;
    }
    return false;
}

// The folders of the user's profile a component of `ckey` lies in, from `dir_key` up to the known
// folder, each get a RemoveFile row (no file name, at removal: the folder when it is empty), as
// ICE64 wants (RFC-0029). A folder that already has such a row of a component removed with the
// product gets none; one whose rows all belong to kept or conditional components gets another.
static void profile_folders(pkg_t *pk, const char *dir_key, const char *ckey) {
    keep_t *k = pk->k;
    bool removed = comp_removed(pk, ckey);
    for (const dnode_t *n = find_node(pk->dirs, dir_key); n && n->long_name && n->parent; n = find_node(pk->dirs, n->parent)) {
        bool covered = false;
        for (size_t r = 0; r + 5 <= pk->removefile.filled && !covered; r += 5) {
            const rp_msi_cell_t *c = &pk->removefile.cells[r];
            if (c[2].kind != RP_MSI_NULL || c[3].len != strlen(n->key) || memcmp(c[3].bytes, n->key, c[3].len) != 0) continue;
            char owner[73];
            snprintf(owner, sizeof owner, "%.*s", (int)c[1].len, (const char *)c[1].bytes);
            covered = !removed || comp_removed(pk, owner);
        }
        if (covered) continue;
        char rk[23];
        rp_key_derive('R', kprintf(k, "profile-folder:%s|%s", n->key, ckey), rk);
        s_(&pk->removefile, kdup(k, rk)); s_(&pk->removefile, ckey); null_(&pk->removefile);
        s_(&pk->removefile, n->key); i_(&pk->removefile, 2);
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
        // In the user's profile: an HKCU key path, and so a GUID of its own (RFC-0029). A GUID the
        // source fixed keeps the file as the key path (RP1320).
        bool profile = lf->f->component_guid == NULL && dir_in_profile(ir, lf->f->dir);
        if (lf->f->component_guid) {
            snprintf(guid, sizeof guid, "%s", lf->f->component_guid);
        } else {
            const char *logical = kprintf(k, "%s/%s", lf->f->dir, lf->f->name);
            const char *fields[] = { ir->upgrade_code, "machine", arch_text(ir->arch), logical, profile ? "file-hkcu" : "file", lf->key };
            rp_uuid_derive("rubrapack.component", fields, 6, guid);
        }
        s_(&pk->component, lf->comp);
        s_(&pk->component, kdup(k, guid));
        s_(&pk->component, lf->dir_key);
        i_(&pk->component, pk->comp_attr | (lf->f->keep ? 16 : 0) | (profile ? 4 : 0));     // keep: Permanent, left at removal (RFC-0013 A7)
        s_(&pk->component, lf->f->when);                            // RFC-0013 A2
        s_(&pk->component, profile ? profile_keypath(pk, lf->comp) : lf->key);
        if (profile || dir_in_profile(ir, lf->f->dir)) profile_folders(pk, lf->dir_key, lf->comp);      // ICE64, a fixed GUID too

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

static int cmp_cstr(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }

// One field of a helper DLL plan (RFC-0001 9.6.1): "<UTF-16 length>:" and the text.
static const char *plan_field(keep_t *k, const char *plan, const char *field) {
    rp_text_result_t u = rp_utf8_to_utf16((const uint8_t *)field, strlen(field), NULL, 0);
    char head[24];
    snprintf(head, sizeof head, "%zu:", u.units);
    return kprintf(k, "%s%s", kprintf(k, "%s%s", plan, head), field);
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
        if (r->purge) {         // the helper DLL's, at a real removal (RP_PURGE: k, root, view, key, component;
                                // K: every user's too, when the removal is told so - RFC-0030)
            static const char *const proots[] = { "HKMU", "HKCR", "HKCU", "HKLM" };
            const char *fields[] = { r->users_all ? "K" : "k", proots[r->root + 1], r->view32 || ir->arch == RP_ARCH_X86 ? "32" : "64", r->key, ckey };
            for (size_t f = 0; f < sizeof fields / sizeof fields[0]; ++f) pk->pplan = plan_field(k, pk->pplan, fields[f]);
            pk->any_purge = true;
            pk->any_others |= r->users_all;
            continue;
        }
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
            for (size_t f = 0; f < sizeof fields / sizeof fields[0]; ++f) pk->qplan = plan_field(k, pk->qplan, fields[f]);
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

// [package] parent (x46): an add-on writes HKLM (per user HKCU, dual HKMU)
// SOFTWARE\rubrapack\Addons\<parent's UpgradeCode>, value <its ProductCode> = <its name>, in a
// component of its own in every feature, so the value is there exactly while the add-on is installed.
// The main product's removal reads it (RpCleanupPrepare, remove-addons = true).
static void lower_addon(pkg_t *pk) {
    const rp_ir_t *ir = pk->ir;
    keep_t *k = pk->k;
    if (ir->parent == NULL || ir->module) return;
    char comp[23], guid[39];
    rp_key_derive('C', "addon:parent", comp);
    const char *fields[] = { ir->upgrade_code, "machine", arch_text(ir->arch), ir->parent, "addon", "parent" };
    rp_uuid_derive("rubrapack.component", fields, 6, guid);
    const char *ckey = kdup(k, comp);
    s_(&pk->component, ckey); s_(&pk->component, kdup(k, guid)); s_(&pk->component, "TARGETDIR");
    i_(&pk->component, 4 | (ir->arch != RP_ARCH_X86 ? 256 : 0)); null_(&pk->component); s_(&pk->component, "RP_AddonParent");
    for (size_t i = 0; i < ir->feature_count; ++i) {
        s_(&pk->featurecomp, ir->features[i].id); s_(&pk->featurecomp, ckey);
    }
    s_(&pk->registry, "RP_AddonParent"); i_(&pk->registry, ir->scope == 0 ? 2 : ir->scope == 1 ? 1 : -1);
    s_(&pk->registry, kprintf(k, "SOFTWARE\\rubrapack\\Addons\\%s", ir->parent, NULL));
    s_(&pk->registry, "[ProductCode]"); s_(&pk->registry, "[ProductName]"); s_(&pk->registry, ckey);
    pk->any_write = true;
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
        // Target: an advertised shortcut names the feature (Windows installs it on demand and
        // repairs it when launched, RFC-0031 B8); an ordinary one names the target file's path.
        if (sc->advertise) s_(&pk->shortcut, target->f->feature);
        else s_(&pk->shortcut, kprintf(k, "[%s]%s", target->dir_key, escape_formatted(k, target->f->name)));
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
// A [remove] name for RemoveFile.FileName, whose type wants a short (8.3) name, or "short|long"
// (ICE03 otherwise: "*.cache"). The short pattern keeps the wildcards and cuts the rest to 8.3; a
// volume without long names is the only place it would be used.
static const char *remove_name(keep_t *k, const char *name) {
    if (name == NULL) return NULL;
    const char *dot = strrchr(name, '.');
    size_t nb = dot ? (size_t)(dot - name) : strlen(name), ne = dot ? strlen(dot + 1) : 0;
    bool ok = nb >= 1 && nb <= 8 && ne <= 3 && !(dot && ne == 0) && memchr(name, '.', nb) == NULL;
    char sh[13];
    size_t n = 0;
    for (size_t i = 0; name[i]; ++i) {
        unsigned char ch = (unsigned char)name[i];
        bool plain = (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch && strchr("*?!#$%&'()-@^_`{}~", ch));
        if (name + i == dot) {
            if (ne) { sh[n++] = '.'; }
            continue;
        }
        if (!plain) ok = false;
        size_t at = dot && name + i > dot ? (size_t)(name + i - dot - 1) : i;
        if (at < (dot && name + i > dot ? 3u : 8u)) sh[n++] = plain ? (char)(ch >= 'a' && ch <= 'z' ? ch - 32 : ch) : '_';
    }
    sh[n] = 0;
    if (ok) return name;
    return kprintf(k, "%s|%s", kdup(k, sh[0] && sh[0] != '.' ? sh : "_"), name);
}

static void lower_removes(pkg_t *pk) {
    const rp_ir_t *ir = pk->ir;
    keep_t *k = pk->k;
    for (size_t i = 0; i < ir->remove_count; ++i) {
        const rp_ir_remove_t *r = &ir->removes[i];
        char comp[23], guid[39];
        rp_key_derive('C', kprintf(k, "remove:%s", r->id, NULL), comp);
        const char *logical = kprintf(k, "%s/%s", r->dir, r->name ? r->name : "");
        // A recursive remove of a folder in the user's profile: its component sits at TARGETDIR, as
        // the registry ones, and creates no folder - a component in the profile wants an HKCU key
        // path (ICE38). Elsewhere the folder is made at installation, by the installer, as before;
        // in the profile with an HKCU key path (RFC-0029).
        bool aside = r->recursive && dir_in_profile(ir, r->dir);
        bool profile = !aside && dir_in_profile(ir, r->dir);
        const char *fields[] = { ir->upgrade_code, "machine", arch_text(ir->arch), logical, profile ? "remove-hkcu" : "remove", r->id };
        rp_uuid_derive("rubrapack.component", fields, 6, guid);
        const char *ckey = kdup(k, comp);
        s_(&pk->component, ckey); s_(&pk->component, kdup(k, guid)); s_(&pk->component, aside ? "TARGETDIR" : dkey(ir, r->dir));
        i_(&pk->component, (ir->arch != RP_ARCH_X86 ? 256 : 0) | (profile ? 4 : 0)); null_(&pk->component);
        if (profile) s_(&pk->component, profile_keypath(pk, ckey)); else null_(&pk->component);
        s_(&pk->featurecomp, r->feature); s_(&pk->featurecomp, ckey);
        if (!aside) { s_(&pk->createfolder, dkey(ir, r->dir)); s_(&pk->createfolder, ckey); }       // its key path is the folder (ICE18)
        if (profile) profile_folders(pk, dkey(ir, r->dir), ckey);
        // upgrade = false: the removal part is the helper DLL's (RP_REMOVES: Directory key, pattern or
        // "" for the folder itself, component), which skips it when an upgrade removes this version;
        // the RemoveFile row keeps only the installation part.
        int mode = r->mode;
        if (r->recursive) {     // the helper DLL's, at a real removal (RP_PURGE: d, Directory key, "", "", component;
                                // D: every user's same folder too, when the removal is told so - RFC-0030)
            const char *fields[] = { r->users_all ? "D" : "d", dkey(ir, r->dir), "", "", ckey };
            for (size_t f = 0; f < sizeof fields / sizeof fields[0]; ++f) pk->pplan = plan_field(k, pk->pplan, fields[f]);
            pk->any_purge = true;
            pk->any_others |= r->users_all;
            // The folder itself, when it is left empty, also goes the installer's way: a folder of
            // the user's profile has to be in RemoveFile (ICE64). Nothing in it is touched by this.
            s_(&pk->removefile, r->id); s_(&pk->removefile, ckey); null_(&pk->removefile); s_(&pk->removefile, dkey(ir, r->dir));
            i_(&pk->removefile, 2);
            continue;
        }
        if (r->keep_on_upgrade) {
            const char *fields[] = { dkey(ir, r->dir), r->name ? r->name : "", ckey };
            for (size_t f = 0; f < sizeof fields / sizeof fields[0]; ++f) pk->rplan = plan_field(k, pk->rplan, fields[f]);
            pk->any_rplan = true;
            mode &= 1;
        }
        if (mode == 0) continue;
        s_(&pk->removefile, r->id); s_(&pk->removefile, ckey); s_(&pk->removefile, remove_name(k, r->name)); s_(&pk->removefile, dkey(ir, r->dir));
        i_(&pk->removefile, mode);
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
        bool profile = dir_in_profile(ir, x->dir);                 // RFC-0029: an HKCU key path
        const char *fields[] = { ir->upgrade_code, "machine", arch_text(ir->arch), logical, profile ? "ini-hkcu" : "ini", x->id };
        rp_uuid_derive("rubrapack.component", fields, 6, guid);
        const char *ckey = kdup(k, comp);
        s_(&pk->component, ckey); s_(&pk->component, kdup(k, guid)); s_(&pk->component, dkey(ir, x->dir));
        i_(&pk->component, (ir->arch != RP_ARCH_X86 ? 256 : 0) | (profile ? 4 : 0)); s_(&pk->component, x->when);
        if (profile) s_(&pk->component, profile_keypath(pk, ckey)); else null_(&pk->component);
        s_(&pk->featurecomp, x->feature); s_(&pk->featurecomp, ckey);
        s_(&pk->createfolder, dkey(ir, x->dir)); s_(&pk->createfolder, ckey);     // its key path is the folder (ICE18)
        if (profile) profile_folders(pk, dkey(ir, x->dir), ckey);
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
// The languages a package carries its helper texts in: English, the built-in packs, and the
// dialogs' own. Each once, English first.
static size_t text_langs(const rp_ir_t *ir, const char *codes[RP_UI_LANG_MAX + 64]) {
    size_t n = 0;
    codes[n++] = "en";
    for (size_t i = 0; i < rp_lang_embedded_count && n < RP_UI_LANG_MAX + 64; ++i) codes[n++] = rp_lang_embedded[i].code;
    for (size_t u = 0; u < ir->ui_lang_count && n < RP_UI_LANG_MAX + 64; ++u) {
        bool have = false;
        for (size_t j = 0; j < n; ++j) have |= strcmp(codes[j], ir->ui_langs[u].code) == 0;
        if (!have) codes[n++] = ir->ui_langs[u].code;
    }
    return n;
}

// The language code of the package's own language ([package] language, a LANGID): a dialog
// language or a built-in pack that names it, else English.
static const char *package_lang(const rp_ir_t *ir) {
    for (size_t u = 0; u < ir->ui_lang_count; ++u) {
        for (size_t j = 0; j < ir->ui_langs[u].langid_count; ++j) {
            if (ir->ui_langs[u].langids[j] == ir->language) return ir->ui_langs[u].code;
        }
    }
    for (size_t i = 0; i < rp_lang_embedded_count; ++i) {
        const rp_langpack_t *p = rp_langpack_get(rp_lang_embedded[i].code, NULL);
        for (size_t j = 0; p && j < p->langid_count; ++j) {
            if (p->langids[j] == ir->language) return p->code;
        }
    }
    return "en";
}

static void lower_helper_actions(pkg_t *pk) {
    const char *langs[RP_UI_LANG_MAX + 64];
    size_t nlangs = text_langs(pk->ir, langs);
    const rp_ir_t *ir = pk->ir;
    keep_t *k = pk->k;
    rp_srcdiags_t *diags = pk->diags;
    const char *guard = NULL;
    for (size_t i = 0; i < ir->dir_count; ++i) {
        if (ir->dirs[i].guard) guard = guard ? kprintf(k, "%s;%s", guard, dkey(ir, ir->dirs[i].id)) : dkey(ir, ir->dirs[i].id);
    }
    // The last pages' Save log button (RFC-0020): Windows Installer keeps a log of every run
    // (MsiLogging, unless /l names one), and RpSaveLog copies it where the user says.
    bool save_log = ir->ui_save_log && ir->ui != RP_UI_NONE;
    const unsigned char *part = ir->arch == RP_ARCH_X64 ? rp_ca_x64 : ir->arch == RP_ARCH_X86 ? rp_ca_x86 : rp_ca_arm64;
    size_t part_len = ir->arch == RP_ARCH_X64 ? rp_ca_x64_len : ir->arch == RP_ARCH_X86 ? rp_ca_x86_len : rp_ca_arm64_len;
    // RFC-0026: every package registers a cleanup task when it leaves something for the next
    // restart, unless cleanup = false (or this rubrapack has no helper DLL, which only it needs).
    bool cleanup = !ir->no_cleanup && !ir->module && part_len > 0;
    bool preflight = !ir->no_preflight && !ir->module && part_len > 0;
    // [menu.*] for the Windows 11 menu: the identity package is registered by the helper DLL (the
    // model has its two files once the build command made them).
    bool menu = false;
    for (size_t i = 0; i < ir->file_count; ++i) menu |= ir->menu_package && strcmp(ir->files[i].id, "RpMenuPkg") == 0;
    menu = menu && part_len > 0;
    pk->menu = menu;
    // A package without dialogs: the helper DLL asks an installed product's user what to do
    // (RpMaintain); the dialog sets have RpMaintenanceDlg.
    bool maintain = ir->ui == RP_UI_NONE && !ir->module && part_len > 0;
    // RFC-0003 6 / RFC-0031 B31: with [action]s, a rollback action that runs last in a rollback
    // looks at the end state and writes the double-failure report when it is not what was there.
    bool report = false;
    for (size_t i = 0; i < ir->action_count; ++i) report |= ir->actions[i].kind == RP_ACT_PAIR;
    report = report && !ir->module && part_len > 0;
    pk->report = report;
    // RP_LANGS: which language a user's LANGID means ("1042=ko;1041=ja"), for a run in which no
    // dialog chose one - the helper's messages then follow the user's language. The first match
    // wins, so the dialogs' own languages come before the other built-in packs.
    if ((preflight || maintain || report) && nlangs > 1) {
        const char *map = "";
        for (size_t i = 0; i < 2 * (nlangs - 1); ++i) {        // two passes over langs[1..]
            size_t pass = i / (nlangs - 1), b = 1 + i % (nlangs - 1);
            const rp_langpack_t *p = rp_langpack_get(langs[b], NULL);
            const uint16_t *own = NULL;
            size_t nown = 0;
            for (size_t u = 0; u < ir->ui_lang_count; ++u) {
                if (strcmp(ir->ui_langs[u].code, langs[b]) == 0) { own = ir->ui_langs[u].langids; nown = ir->ui_langs[u].langid_count; }
            }
            if ((own != NULL) != (pass == 0)) continue;
            for (size_t j = 0; j < (own ? nown : p ? p->langid_count : 0); ++j) {
                char num[16];
                snprintf(num, sizeof num, "%u", own ? (unsigned)own[j] : p->langids[j]);
                map = kprintf(k, "%s%s", map, kprintf(k, "%s=%s;", kdup(k, num), langs[b]));
            }
        }
        if (*map) { s_(&pk->property, "RP_LANGS"); s_(&pk->property, map); }
    }
    // [ui] details: the progress page's box, filled by the helper DLL from the log as it grows.
    bool details = ir->ui_details && ir->ui != RP_UI_NONE && !ir->module && part_len > 0;
    pk->details = details;
    // RFC-0028: past RP_UI_LANG_ACTIONS the dialogs' texts are put in place by RpLangApply (ui.c).
    bool lang_table = ir->ui != RP_UI_NONE && ir->ui_lang_count > RP_UI_LANG_ACTIONS;
    if (pk->any_qword || pk->any_rplan || pk->any_purge || guard || save_log || cleanup || preflight || menu || maintain || details || lang_table || report) {
        if (part_len == 0) {
            rp_srcdiag_add(diags, (rp_pos_t){ 1, 1 }, "RP1901", false,
                           "%s needs resources/bin/rubrapack_ca-%s.dll, which this rubrapack was built without",
                           pk->any_qword ? "type = \"qword\"" : pk->any_rplan ? "upgrade = false" : pk->any_purge ? "recursive = true" : guard ? "guard = true"
                           : lang_table ? "[ui] languages past seven" : "[ui] save-log",
                           arch_text(ir->arch));
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
    // The install folder page's question about a folder that holds something (ui.c, RpDirWarnDlg):
    // RP_DirCheck reads the folder the property named in RP_DIRCHECK holds.
    if (pk->any_purge && ir->ui >= RP_UI_INSTALLDIR && rp_ui_wipes_dir(ir, ir->ui_install_dir ? ir->ui_install_dir : "INSTALLDIR")) {
        s_(&pk->property, "RP_DIRCHECK"); s_(&pk->property, dkey(ir, ir->ui_install_dir ? ir->ui_install_dir : "INSTALLDIR"));
        s_(&pk->customaction, "RP_DirCheck"); i_(&pk->customaction, 1); s_(&pk->customaction, "RpCa"); s_(&pk->customaction, "RpDirCheck");
    }
    if (details) {      // "!": each line is written at once, so the box follows the installation
        s_(&pk->property, "MsiLogging"); s_(&pk->property, "voicewarmup!");
        // The readable lines' labels, per language (RpDet_<operation>_<code>; a pack's "Details.<operation>").
        static const struct { const char *op, *en; } det[] = {
        { "FileCopy", "Copy file" },
        { "FileRemove", "Remove file" },
        { "FilePatch", "Patch file" },
        { "FolderCreate", "Create folder" },
        { "FolderRemove", "Remove folder" },
        { "RegAddValue", "Write registry value" },
        { "RegRemoveValue", "Remove registry value" },
        { "RegRemoveKey", "Remove registry key" },
        { "ShortcutCreate", "Create shortcut" },
        { "ShortcutRemove", "Remove shortcut" },
        { "ServiceInstall", "Install service" },
        { "ServiceControl", "Control service" },
        { "IniWriteRemoveValue", "Write INI value" },
        { "UpdateEnvironmentStrings", "Set environment variable" },
        { "RegisterFont", "Register font" },
        { "UnregisterFont", "Unregister font" },
        { "CustomActionSchedule", "Run action" },
        };
        for (size_t t = 0; t < sizeof det / sizeof det[0]; ++t) {
            for (size_t b = 0; b < nlangs; ++b) {
                const char *tr = strcmp(langs[b], "en") == 0 ? NULL : rp_langpack_text(rp_langpack_get(langs[b], NULL), "installer", kprintf(k, "Details.%s", det[t].op, NULL));
                if (tr == NULL && b) continue;          // English is what a language without it shows
                s_(&pk->property, kprintf(k, "RpDet_%s_%s", det[t].op, langs[b]));
                s_(&pk->property, tr ? tr : det[t].en);
            }
        }
        // The box's face per dialog language (the language's own: Segoe UI draws no CJK, and the CJK
        // a fallback picks may draw the backslash as a currency sign).
        for (size_t li = 0; li < (ir->ui_lang_count ? ir->ui_lang_count : 1); ++li) {
            const char *code = ir->ui_lang_count ? ir->ui_langs[li].code : "en";
            const char *face = ir->ui_lang_count && ir->ui_langs[li].font ? ir->ui_langs[li].font : "Segoe UI";
            s_(&pk->property, kprintf(k, "RpDetFont_%s", code, NULL)); s_(&pk->property, face);
            // ... and its first LANGID, which gives the helper the language's character set.
            unsigned id = ir->ui_lang_count && ir->ui_langs[li].langid_count ? ir->ui_langs[li].langids[0] : 1033;
            char num[16];
            snprintf(num, sizeof num, "%u", id);
            s_(&pk->property, kprintf(k, "RpDetLcid_%s", code, NULL)); s_(&pk->property, kdup(k, num));
        }
        s_(&pk->customaction, "RP_Details"); i_(&pk->customaction, 1 | 0x80); s_(&pk->customaction, "RpCa"); s_(&pk->customaction, "RpDetails");
        s_(&pk->customaction, "RP_DetailsDone"); i_(&pk->customaction, 1); s_(&pk->customaction, "RpCa"); s_(&pk->customaction, "RpDetailsDone");
    }
    if (save_log) {
        if (!details) { s_(&pk->property, "MsiLogging"); s_(&pk->property, "voicewarmup"); }
        size_t nl = ir->ui_lang_count ? ir->ui_lang_count : 1;
        for (size_t li = 0; li < nl; ++li) {
            s_(&pk->property, kprintf(k, "RpLogMsg_%s", ir->ui_lang_count ? ir->ui_langs[li].code : "en", NULL));
            s_(&pk->property, rp_ui_text_for(ir, "SaveLogFail", li));
        }
        s_(&pk->customaction, "RP_SaveLog"); i_(&pk->customaction, 1); s_(&pk->customaction, "RpCa"); s_(&pk->customaction, "RpSaveLog");
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
    // [menu.*] with text-xx: the classic menu's text is one registry value, so it is the text of the
    // installation's language - RpMenuT_<ID>, set here (English first, then the chosen language:
    // RPLANGUAGE, else the user's display language) and named by the MUIVerb value.
    for (size_t m = 0; m < ir->menu_count; ++m) {
        const rp_ir_menu_t *x = &ir->menus[m];
        if (x->text_by_lang_count == 0) continue;
        const char *prop = kprintf(k, "RpMenuT_%s", x->id, NULL);
        for (size_t w = 0; w <= x->text_by_lang_count; ++w) {
            const char *code = w ? x->text_by_lang[w - 1].lang : "en";
            const char *text = w ? x->text_by_lang[w - 1].text : x->text;
            if (text == NULL) text = x->text_by_lang[0].text;       // no English text: the first language's
            if (w && strcmp(code, "en") == 0) continue;
            // As a Formatted text that stays as written.
            rp_buf_t lit = rp_buf_new(k->alloc, 1u << 16);
            for (const char *p = text; *p; ++p) {
                if (*p == '[' || *p == ']') {
                    rp_buf_puts(&lit, "[\\");
                    rp_buf_byte(&lit, (uint8_t)*p);
                    rp_buf_byte(&lit, ']');
                } else {
                    rp_buf_byte(&lit, (uint8_t)*p);
                }
            }
            rp_buf_byte(&lit, 0);
            const char *cond = NULL;
            if (w) {
                cond = kprintf(k, "RPLANGUAGE = \"%s\"", code, NULL);
                for (size_t li = 1; li < ir->ui_lang_count; ++li) {
                    const rp_ir_ui_lang_t *L = &ir->ui_langs[li];
                    if (strcmp(L->code, code) != 0) continue;
                    for (size_t j = 0; j < L->langid_count; ++j) {
                        char id[16];
                        snprintf(id, sizeof id, "%u", (unsigned)L->langids[j]);
                        cond = kprintf(k, "%s OR (NOT RPLANGUAGE AND UserLanguageID = %s)", cond, id);
                    }
                }
            }
            const char *action = kprintf(k, "RpMenuT_%s_%s", x->id, code);
            s_(&pk->customaction, action); i_(&pk->customaction, 51); s_(&pk->customaction, prop);
            s_(&pk->customaction, lit.err == PROVEN_OK ? kprintf(k, "%s", (const char *)lit.data, NULL) : text);
            s_(&pk->iexec, action); s_(&pk->iexec, cond); i_(&pk->iexec, w ? 1013 : 1012);
            rp_buf_free(&lit);
        }
    }
    // The identity package (rp_appx.h): registered after the files and registry values are there,
    // removed before they go; each with its opposite as the rollback. The actions get
    // "<file>|RPM1|scope|u or s|full name|family name|<classic verb keys>": the file's path formatted
    // by a type-51 action, the rest from the property RpMenuData (a Target holds 255 characters).
    if (menu) {
        static const char *const arch[] = { "x64", "arm64", "x86" };
        char name[160], pub[8400], id[14], version[32];
        unsigned v[4] = { 0 };
        for (size_t i = 0; i < ir->version_count && i < 4; ++i) v[i] = ir->version_parts[i];
        snprintf(version, sizeof version, "%u.%u.%u.%u", v[0], v[1], v[2], v[3]);
        rp_msix_sparse_identity(ir, ir->menu_publisher, name, pub);
        rp_msix_publisher_id(pub, id);
        const char *full = kprintf(k, "%s_%s", name, kprintf(k, "%s_%s", version, kprintf(k, "%s__%s", arch[ir->arch], id)));
        const char *family = kprintf(k, "%s_%s", name, id);
        const char *data = kprintf(k, "RPM1|%s|%s", ir->scope == 0 ? "m" : "u",
                                   kprintf(k, "%s|%s", ir->menu_publisher ? "s" : "u", kprintf(k, "%s|%s", full, family)));
        // Then the classic verbs of the same items (their keys below Software\Classes): where the
        // package is registered Windows shows its items in the classic menu too, so the registry
        // verbs are switched off there (LegacyDisable) - or each item would be there twice.
        for (size_t m = 0; m < ir->menu_count; ++m) {
            const rp_ir_menu_t *x = &ir->menus[m];
            if (x->parent || !x->windows11) continue;
            const char *prefix = kprintf(k, "%s.Verb", x->id, NULL);
            for (size_t r = 0; r < ir->registry_count; ++r) {
                const rp_ir_registry_t *g = &ir->registries[r];
                if (g->root != RP_ROOT_HKCR || g->name == NULL || strcmp(g->name, "MUIVerb") != 0) continue;
                if (strncmp(g->id, prefix, strlen(prefix)) != 0) continue;
                data = kprintf(k, "%s|%s", data, g->key);
            }
        }
        s_(&pk->property, "RpMenuData"); s_(&pk->property, data);
        const int noimp = ir->scope == 0 ? 0x800 : 0;
        static const struct { const char *action, *entry; int type; } acts[] = {
            { "RP_MenuRegisterRollback", "RpMenuRemove", 0x100 }, { "RP_MenuRegister", "RpMenuRegister", 0 },
            { "RP_MenuRemoveRollback", "RpMenuRegister", 0x100 }, { "RP_MenuRemove", "RpMenuRemove", 0 } };
        for (size_t i = 0; i < 4; ++i) {
            const char *set = kprintf(k, "RP_MenuData%s", i == 0 ? "1" : i == 1 ? "2" : i == 2 ? "3" : "4", NULL);
            s_(&pk->customaction, set); i_(&pk->customaction, 51); s_(&pk->customaction, acts[i].action); s_(&pk->customaction, "[#RpMenuPkg]|[RpMenuData]");
            s_(&pk->customaction, acts[i].action); i_(&pk->customaction, 1 | 0x40 | 0x400 | acts[i].type | noimp);
            s_(&pk->customaction, "RpCa"); s_(&pk->customaction, acts[i].entry);
        }
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
    // DECISIONS 2026-10-06: RpPreflight before InstallValidate - the programs using the product's
    // files (asked about, or RPCLOSE), the folders and the older versions' cached packages. Its texts
    // in English, Korean and the dialogs' languages.
    if (preflight) {
        const char *dirs = "";
        const char **keys = rp_mem_alloc(pk->alloc, ir->dir_count + ir->folder_count + 1, sizeof *keys);
        size_t nkeys = 0;
        for (size_t i = 0; keys && i < ir->dir_count; ++i) {
            if (ir->dirs[i].part_count) keys[nkeys++] = dkey(ir, ir->dirs[i].id);
        }
        // A [folder.*] is the product's own too, not "not installed with it" (x67).
        for (size_t i = 0; keys && i < ir->folder_count; ++i) keys[nkeys++] = ir->folders[i].id;
        if (nkeys) rp_sort(keys, nkeys, sizeof *keys, cmp_cstr);
        for (size_t i = 0; i < nkeys; ++i) dirs = *dirs ? kprintf(k, "%s;%s", dirs, keys[i]) : keys[i];
        if (keys) rp_mem_free(pk->alloc, keys);
        if (*dirs) { s_(&pk->property, "RP_PREFLIGHT_DIRS"); s_(&pk->property, dirs); }
        static const char *const modes[] = { "ask", "always", "never" };
        s_(&pk->property, "RP_PREFLIGHT_CLOSE"); s_(&pk->property, modes[ir->close_programs]);
        // DECISIONS 2026-10-14: at a real removal, what is in these folders but was not installed
        // with the product - asked about, or RPEXTRA; "delete" goes the purge's way (RP_PURGE).
        if (*dirs && ir->extra_files != 1) {
            s_(&pk->property, "RP_PREFLIGHT_EXTRA"); s_(&pk->property, ir->extra_files == 2 ? "delete" : "ask");
            pk->extra = true;
        }
        // RFC-0030: "Users" asks whether the other users' settings and data go too.
        bool others = pk->any_others;
        static const char *const ids[] = { "Ask", "Silent", "Folder", "Cache", "Extra", "ExtraSilent", "Users" };
        for (size_t t = 0; t < sizeof ids / sizeof ids[0]; ++t) {
            if (strcmp(ids[t], "Users") == 0 && !others) continue;
            const char *id = kprintf(k, "Preflight%s", ids[t], NULL);
            for (size_t b = 0; b < nlangs; ++b) {       // English, the built-in packs, the dialogs' languages
                s_(&pk->property, kprintf(k, "RpPre%s_%s", ids[t], langs[b]));
                s_(&pk->property, rp_ui_text_lang(ir, id, langs[b]));
            }
        }
        // [other-copy.*]: "kind|action|ID|what" for each, kind m (an MSIX family for the installing
        // user) or u (another MSI's upgrade code), action w(arn) or r(efuse); the texts it needs.
        const char *other = "";
        bool used[4] = { false };
        for (size_t o = 0; o < ir->other_count; ++o) {
            const rp_ir_other_t *x = &ir->others[o];
            const char *what = x->upgrade_code;
            if (x->msix) {
                char id[14];
                rp_msix_publisher_id(x->publisher, id);
                what = kprintf(k, "%s_%s", x->msix, id);
            }
            const char *one = kprintf(k, "%s|%s", x->msix ? "m" : "u", kprintf(k, "%s|%s", x->refuse ? "r" : "w", kprintf(k, "%s|%s", x->id, what)));
            other = *other ? kprintf(k, "%s;%s", other, one) : one;
            if (x->message == NULL) used[(x->msix ? 0 : 2) + (x->refuse ? 1 : 0)] = true;     // else the author's (in English too)
            if (x->message) {
                s_(&pk->property, kprintf(k, "RpPreOther_%s_en", x->id, NULL)); s_(&pk->property, x->message);
            }
            for (size_t w = 0; w < x->message_by_lang_count; ++w) {
                if (x->message && strcmp(x->message_by_lang[w].lang, "en") == 0) continue;
                s_(&pk->property, kprintf(k, "RpPreOther_%s_%s", x->id, x->message_by_lang[w].lang));
                s_(&pk->property, x->message_by_lang[w].text);
            }
        }
        if (*other) {
            s_(&pk->property, "RP_OTHER"); s_(&pk->property, other);
            static const char *const oids[] = { "OtherStore", "OtherStoreRefuse", "OtherMsi", "OtherMsiRefuse" };
            for (size_t t = 0; t < 4; ++t) {
                if (!used[t]) continue;
                const char *id = kprintf(k, "Preflight%s", oids[t], NULL);
                for (size_t b = 0; b < nlangs; ++b) {
                    s_(&pk->property, kprintf(k, "RpPre%s_%s", oids[t], langs[b]));
                    s_(&pk->property, rp_ui_text_lang(ir, id, langs[b]));
                }
            }
        }
        s_(&pk->customaction, "RP_Preflight"); i_(&pk->customaction, 1); s_(&pk->customaction, "RpCa"); s_(&pk->customaction, "RpPreflight");
        pk->preflight = true;
    }
    if (report) {
        // RP_REPORT: a line per [action] with check - its program's file key, a tab, the check's
        // arguments (formatted by RpReportPrepare).
        const char *checks = "";
        for (size_t i = 0; i < ir->action_count; ++i) {
            const rp_ir_action_t *a = &ir->actions[i];
            if (a->kind != RP_ACT_PAIR || a->check_args == NULL || a->run_file == NULL) continue;
            checks = kprintf(k, "%s%s", checks, kprintf(k, "%s\t%s\n", a->run_file, a->check_args));     // the File key is the ID
        }
        if (*checks) { s_(&pk->property, "RP_REPORT"); s_(&pk->property, checks); }
        static const char *const rids[] = { "ReportInstall", "ReportUpgrade", "ReportRemove", "ReportRepair" };
        for (size_t t = 0; t < sizeof rids / sizeof rids[0]; ++t) {
            for (size_t b = 0; b < nlangs; ++b) {
                s_(&pk->property, kprintf(k, "RpPre%s_%s", rids[t], langs[b]));
                s_(&pk->property, rp_ui_text_lang(ir, rids[t], langs[b]));
            }
        }
        s_(&pk->customaction, "RP_ReportPrepare"); i_(&pk->customaction, 1); s_(&pk->customaction, "RpCa"); s_(&pk->customaction, "RpReportPrepare");
        s_(&pk->customaction, "RP_Report"); i_(&pk->customaction, 1 | 0x40 | 0x100 | 0x400); s_(&pk->customaction, "RpCa"); s_(&pk->customaction, "RpReport");
    }
    if (maintain) {
        static const char *const ids[] = { "MaintTitle", "MaintText", "Repair", "Remove", "Cancel" };
        for (size_t t = 0; t < sizeof ids / sizeof ids[0]; ++t) {
            for (size_t b = 0; b < nlangs; ++b) {
                s_(&pk->property, kprintf(k, "RpPre%s_%s", ids[t], langs[b]));
                s_(&pk->property, rp_ui_text_lang(ir, ids[t], langs[b]));
            }
        }
        for (size_t b = 0; b < nlangs; ++b) {   // the second question before a removal
            s_(&pk->property, kprintf(k, "RpPreRemoveAsk_%s", langs[b], NULL));
            s_(&pk->property, rp_ui_text_lang(ir, rp_ui_wipes(ir) ? "RemoveAskAll" : "RemoveAsk", langs[b]));
        }
        s_(&pk->customaction, "RP_Maintain"); i_(&pk->customaction, 1); s_(&pk->customaction, "RpCa"); s_(&pk->customaction, "RpMaintain");
        pk->maintain = true;
    }
    // RFC-0026: the package's folders and scope for the commit action, which registers the cleanup
    // task only when the installation left something queued for the next restart.
    if (cleanup) {
        const char *dirs = "";
        // Only folders the package makes below a standard one: a [dir] that is the standard folder
        // itself (the Desktop, the Start menu) is never the cleanup task's to remove.
        // Sorted, so a source written back by `new --from` builds the same bytes.
        const char **keys = rp_mem_alloc(pk->alloc, ir->dir_count + 1, sizeof *keys);
        size_t nkeys = 0;
        for (size_t i = 0; keys && i < ir->dir_count; ++i) {
            if (ir->dirs[i].part_count) keys[nkeys++] = dkey(ir, ir->dirs[i].id);
        }
        if (nkeys) rp_sort(keys, nkeys, sizeof *keys, cmp_cstr);
        for (size_t i = 0; i < nkeys; ++i) dirs = *dirs ? kprintf(k, "%s;%s", dirs, keys[i]) : keys[i];
        if (keys) rp_mem_free(pk->alloc, keys);
        s_(&pk->property, "RP_CLEANUP_DIRS"); s_(&pk->property, *dirs ? dirs : "TARGETDIR");
        s_(&pk->property, "RP_CLEANUP_SCOPE"); s_(&pk->property, ir->scope == 0 ? "machine" : "user");
        if (ir->remove_addons) { s_(&pk->property, "RP_REMOVE_ADDONS"); s_(&pk->property, "1"); }   // x46
        const int noimp = ir->scope == 0 ? 0x800 : 0;
        s_(&pk->customaction, "RP_CleanupPrepare"); i_(&pk->customaction, 1 | 0x40); s_(&pk->customaction, "RpCa");
        s_(&pk->customaction, "RpCleanupPrepare");
        s_(&pk->customaction, "RP_CleanupRegister"); i_(&pk->customaction, 1 | 0x40 | 0x200 | 0x400 | noimp); s_(&pk->customaction, "RpCa");
        s_(&pk->customaction, "RpCleanupRegister");
        pk->cleanup = true;
        // x41: take back the deletions an earlier removal queued for the paths this package installs
        // (per machine: the list is in HKLM).
        if (ir->scope == 0) {
            s_(&pk->customaction, "RP_RescuePrepare"); i_(&pk->customaction, 1 | 0x40); s_(&pk->customaction, "RpCa");
            s_(&pk->customaction, "RpRescuePrepare");
            s_(&pk->customaction, "RP_RescueApplyRollback"); i_(&pk->customaction, 1 | 0x40 | 0x100 | 0x400 | noimp);
            s_(&pk->customaction, "RpCa"); s_(&pk->customaction, "RpRescueRollback");
            s_(&pk->customaction, "RP_RescueApply"); i_(&pk->customaction, 1 | 0x40 | 0x400 | noimp); s_(&pk->customaction, "RpCa");
            s_(&pk->customaction, "RpRescueApply");
        }
    }
    // [remove] upgrade = false: prepare (immediate; nothing when UPGRADINGPRODUCTCODE is set) ->
    // rollback twin -> apply (moves the files to a backup folder) -> commit (deletes the backup).
    if (pk->any_rplan) {
        s_(&pk->property, "RP_REMOVES"); s_(&pk->property, kprintf(k, "%s%s", pk->rplan, NULL));
        s_(&pk->property, "RP_REMOVES_ROOT"); s_(&pk->property, ir->scope == 0 ? "volume" : "temp");
        const int noimp = ir->scope == 0 ? 0x800 : 0;
        s_(&pk->customaction, "RP_RemovePrepare"); i_(&pk->customaction, 1); s_(&pk->customaction, "RpCa"); s_(&pk->customaction, "RpRemovePrepare");
        s_(&pk->customaction, "RP_RemoveApplyRollback"); i_(&pk->customaction, 1 | 0x40 | 0x100 | 0x400 | noimp);
        s_(&pk->customaction, "RpCa"); s_(&pk->customaction, "RpRemoveRollback");
        s_(&pk->customaction, "RP_RemoveApply"); i_(&pk->customaction, 1 | 0x40 | 0x400 | noimp); s_(&pk->customaction, "RpCa");
        s_(&pk->customaction, "RpRemoveApply");
        s_(&pk->customaction, "RP_RemoveCommit"); i_(&pk->customaction, 1 | 0x40 | 0x200 | 0x400 | noimp); s_(&pk->customaction, "RpCa");
        s_(&pk->customaction, "RpRemoveCommit");
    }
    // [remove] recursive, [registry] on = "uninstall": prepare (immediate; nothing when
    // UPGRADINGPRODUCTCODE is set) -> two commit actions, so a removal that fails has touched
    // nothing: one as the user (the user's profile and HKCU), one - per machine - as SYSTEM. The
    // preflight adds the folders whose extra files the user chose to delete (extra-files).
    if (pk->any_purge || pk->extra) {
        s_(&pk->property, "RP_PURGE"); s_(&pk->property, kprintf(k, "%s%s", pk->pplan, NULL));
        // A dual package installed for all users (ALLUSERS=1) wipes its machine part as SYSTEM too (RFC-0031 L4).
        if (ir->scope != 1) { s_(&pk->property, "RP_PURGE_SPLIT"); s_(&pk->property, "1"); }
        s_(&pk->customaction, "RP_PurgePrepare"); i_(&pk->customaction, 1 | 0x40); s_(&pk->customaction, "RpCa"); s_(&pk->customaction, "RpPurgePrepare");
        s_(&pk->customaction, "RP_PurgeUser"); i_(&pk->customaction, 1 | 0x40 | 0x200 | 0x400); s_(&pk->customaction, "RpCa");
        s_(&pk->customaction, "RpPurge");
        if (ir->scope != 1) {
            s_(&pk->customaction, "RP_PurgeMachine"); i_(&pk->customaction, 1 | 0x40 | 0x200 | 0x400 | 0x800); s_(&pk->customaction, "RpCa");
            s_(&pk->customaction, "RpPurge");
        }
        // RFC-0030: the other users' profiles and HKCU, as SYSTEM, when the removal was told so
        // (the preflight's question, or RPPURGEUSERS=all).
        if (pk->any_others) {
            s_(&pk->customaction, "RP_PurgeOthers"); i_(&pk->customaction, 1 | 0x40 | 0x200 | 0x400 | 0x800); s_(&pk->customaction, "RpCa");
            s_(&pk->customaction, "RpPurgeOthers");
        }
    }
    // RFC-0031 L2: with rollback disabled (policy, or a DisableRollback action) Windows Installer runs
    // no commit action. Each of rubrapack's own commit actions (RpCa, RpCleanupRegister/RpPurge/...)
    // has a twin "<name>NR", deferred without the commit flag, run only when RollbackDisabled is set
    // (sequence 6592.., added in lower_sequences); set_data gives it the same CustomActionData. An
    // author's commit action is not twinned - it is skipped when rollback is off, as Windows does.
    const int noimp = ir->scope == 0 ? 0x800 : 0;
    struct { const char *name, *entry; bool have; int type; } commits[] = {
        { "RP_CleanupRegister", "RpCleanupRegister", pk->cleanup, 1 | 0x40 | 0x400 | noimp },
        { "RP_RemoveCommit", "RpRemoveCommit", pk->any_rplan, 1 | 0x40 | 0x400 | noimp },
        { "RP_PurgeUser", "RpPurge", pk->any_purge || pk->extra, 1 | 0x40 | 0x400 },
        { "RP_PurgeMachine", "RpPurge", (pk->any_purge || pk->extra) && ir->scope != 1, 1 | 0x40 | 0x400 | 0x800 },
        { "RP_PurgeOthers", "RpPurgeOthers", pk->any_others, 1 | 0x40 | 0x400 | 0x800 },
    };
    for (size_t i = 0; i < sizeof commits / sizeof commits[0]; ++i) {
        if (!commits[i].have) continue;
        s_(&pk->customaction, kprintf(k, "%sNR", commits[i].name, NULL)); i_(&pk->customaction, commits[i].type);
        s_(&pk->customaction, "RpCa"); s_(&pk->customaction, commits[i].entry);
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
        s_(&pk->launch, rp_ui_text_builtin("PerUserOnly", package_lang(ir)));
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
        if (dir_in_profile(ir, cp->dir)) profile_folders(pk, dkey(ir, cp->dir), src->comp);       // ICE64 (RFC-0029)
    }
}

// RFC-0031 wave 1: MoveFile, the ODBC tables, PublishComponent, IsolatedComponent.
static const lfile_t *lfile_of(const pkg_t *pk, const char *id) {
    for (size_t j = 0; id && j < pk->nfiles; ++j) {
        if (strcmp(pk->files[j].key, id) == 0) return &pk->files[j];
    }
    return NULL;
}

// "SHORT|long" for a name that is not 8.3 already (MoveFile.DestName is a Filename).
static const char *move_name(keep_t *k, const char *name) {
    const char *dot = strrchr(name, '.');
    size_t base = dot ? (size_t)(dot - name) : strlen(name), ext = dot ? strlen(dot + 1) : 0;
    bool plain = base >= 1 && base <= 8 && ext <= 3;
    for (const char *p = name; plain && *p; ++p) plain = (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_' || (p == dot);
    if (plain) return name;
    char sh[16];
    size_t n = 0;
    for (const char *p = name; *p && p != dot && n < 6; ++p) {
        char ch = *p >= 'a' && *p <= 'z' ? (char)(*p - 32) : *p;
        if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_') sh[n++] = ch;
    }
    if (n == 0) sh[n++] = 'F';
    sh[n++] = '~';
    sh[n++] = '1';
    if (dot) {
        sh[n++] = '.';
        for (const char *p = dot + 1, *e = dot + 4; *p && p < e; ++p) {
            char ch = *p >= 'a' && *p <= 'z' ? (char)(*p - 32) : *p;
            if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_') sh[n++] = ch;
        }
        if (sh[n - 1] == '.') --n;
    }
    sh[n] = 0;
    return kprintf(k, "%s|%s", sh, name);
}

// One ODBCAttribute / ODBCSourceAttribute row from "Key=Value" (the IR checked the '=').
static void odbc_attr(keep_t *k, rows_t *t, const char *owner, const char *kv) {
    const char *eq = strchr(kv, '=');
    char key[256];
    size_t n = eq ? (size_t)(eq - kv) : strlen(kv);
    if (n >= sizeof key) n = sizeof key - 1;
    memcpy(key, kv, n);
    key[n] = 0;
    s_(t, owner); s_(t, kdup(k, key)); s_(t, eq ? eq + 1 : NULL);
}

static void lower_more(pkg_t *pk) {
    const rp_ir_t *ir = pk->ir;
    keep_t *k = pk->k;
    for (size_t i = 0; i < ir->move_count; ++i) {
        const rp_ir_move_t *x = &ir->moves[i];
        const char *comp = NULL;
        if (x->with_file) {
            const lfile_t *f = lfile_of(pk, x->with_file);
            comp = f ? f->comp : NULL;
        } else {
            // A component of its own in the destination folder (the folder its key path, ICE18).
            char ckey[23], guid[39];
            rp_key_derive('C', kprintf(k, "move.%s", x->id, NULL), ckey);
            const char *fields[] = { ir->upgrade_code, "machine", arch_text(ir->arch), x->to_dir, "move", x->id };
            rp_uuid_derive("rubrapack.component", fields, 6, guid);
            comp = kdup(k, ckey);
            s_(&pk->component, comp); s_(&pk->component, kdup(k, guid)); s_(&pk->component, dkey(ir, x->to_dir));
            i_(&pk->component, pk->comp_attr); null_(&pk->component); null_(&pk->component);
            s_(&pk->featurecomp, x->feature); s_(&pk->featurecomp, comp);
            s_(&pk->createfolder, dkey(ir, x->to_dir)); s_(&pk->createfolder, comp);
        }
        if (comp == NULL) {
            pk->reg_bad = true;
            continue;
        }
        const char *from = x->from_prop ? x->from_prop : dkey(ir, x->from_dir);
        s_(&pk->movefile, x->id); s_(&pk->movefile, comp); s_(&pk->movefile, x->name);
        s_(&pk->movefile, x->new_name ? move_name(k, x->new_name) : NULL);
        s_(&pk->movefile, from); s_(&pk->movefile, dkey(ir, x->to_dir)); i_(&pk->movefile, x->copy ? 0 : 1);
    }
    for (size_t i = 0; i < ir->odbc_driver_count; ++i) {
        const rp_ir_odbc_driver_t *x = &ir->odbc_drivers[i];
        const lfile_t *f = lfile_of(pk, x->file), *setup = x->setup ? lfile_of(pk, x->setup) : NULL;
        if (f == NULL || (x->setup && setup == NULL)) {
            pk->reg_bad = true;
            continue;
        }
        s_(&pk->odbcdriver, x->id); s_(&pk->odbcdriver, f->comp); s_(&pk->odbcdriver, x->description);
        s_(&pk->odbcdriver, f->key); s_(&pk->odbcdriver, setup ? setup->key : NULL);
        for (size_t a = 0; a < x->attribute_count; ++a) odbc_attr(k, &pk->odbcattr, x->id, x->attributes[a]);
    }
    for (size_t i = 0; i < ir->odbc_source_count; ++i) {
        const rp_ir_odbc_source_t *x = &ir->odbc_sources[i];
        const char *comp = NULL, *dname = x->driver_name;
        for (size_t j = 0; x->driver && j < ir->odbc_driver_count; ++j) {
            if (strcmp(ir->odbc_drivers[j].id, x->driver) != 0) continue;
            const lfile_t *f = lfile_of(pk, ir->odbc_drivers[j].file);
            comp = f ? f->comp : NULL;
            dname = ir->odbc_drivers[j].description;
        }
        if (x->with_file) {
            const lfile_t *f = lfile_of(pk, x->with_file);
            comp = f ? f->comp : NULL;
        }
        if (comp == NULL || dname == NULL) {
            pk->reg_bad = true;
            continue;
        }
        s_(&pk->odbcsource, x->id); s_(&pk->odbcsource, comp); s_(&pk->odbcsource, x->name);
        s_(&pk->odbcsource, dname); i_(&pk->odbcsource, x->user ? 1 : 0);
        for (size_t a = 0; a < x->attribute_count; ++a) odbc_attr(k, &pk->odbcsattr, x->id, x->attributes[a]);
    }
    for (size_t i = 0; i < ir->publish_count; ++i) {
        const rp_ir_publish_t *x = &ir->publishes[i];
        const lfile_t *f = lfile_of(pk, x->file);
        if (f == NULL) {
            pk->reg_bad = true;
            continue;
        }
        s_(&pk->publish, x->category); s_(&pk->publish, x->qualifier); s_(&pk->publish, f->comp);
        s_(&pk->publish, x->data); s_(&pk->publish, f->f->feature);
    }
    for (size_t i = 0; i < ir->file_count; ++i) {
        const rp_ir_file_t *f = &ir->files[i];
        if (f->isolate_for == NULL) continue;
        const lfile_t *dll = lfile_of(pk, f->id), *app = lfile_of(pk, f->isolate_for);
        if (dll == NULL || app == NULL) {
            pk->reg_bad = true;
            continue;
        }
        s_(&pk->isolated, dll->comp); s_(&pk->isolated, app->comp);
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
        bool profile = dir_in_profile(ir, f->dir);                 // RFC-0029: an HKCU key path
        const char *fields[] = { ir->upgrade_code, "machine", arch_text(ir->arch), logical, profile ? "folder-hkcu" : "folder", f->id };
        rp_uuid_derive("rubrapack.component", fields, 6, guid);
        const char *ckey = kdup(k, comp);
        s_(&pk->component, ckey);
        s_(&pk->component, kdup(k, guid));
        s_(&pk->component, f->id);
        i_(&pk->component, pk->comp_attr | (f->keep ? 16 : 0) | (profile ? 4 : 0));
        null_(&pk->component);
        if (profile) s_(&pk->component, profile_keypath(pk, ckey)); else null_(&pk->component);
        s_(&pk->featurecomp, f->feature);
        s_(&pk->featurecomp, ckey);
        s_(&pk->createfolder, f->id);
        s_(&pk->createfolder, ckey);
        if (profile) profile_folders(pk, f->id, ckey);
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

// A refusal's error action (type 19) in the dialogs' language - an older package over a newer one
// spoke the package's language whatever the dialogs' was (x62). An author's text stays one action.
// Past RP_UI_LANG_ACTIONS the text is RpT_<id>, which RpLangApply sets at 19 (ui.c, which registers it); with fewer
// languages each whose text differs gets an action of its own on RPLANGUAGE (settled at 19 in both
// sequences), numbered from `first`, and the package's own text takes the rest (*cond).
static void refusal(pkg_t *pk, const char *name, const char *id, const char *custom, const char *suffix, const char *found, int first,
                    const char **cond) {
    const rp_ir_t *ir = pk->ir;
    keep_t *k = pk->k;
    *cond = found;
    const char *base = custom ? escape_formatted(k, custom) : rp_ui_text_builtin(id, package_lang(ir));
    bool multi = ir->ui && ir->ui_lang_count > 1, table = multi && ir->ui_lang_count > RP_UI_LANG_ACTIONS;    // as ui.c, built later
    if (custom == NULL && table) base = kprintf(k, "[RpT_%s]", id, NULL);
    s_(&pk->customaction, name); i_(&pk->customaction, 19); null_(&pk->customaction); s_(&pk->customaction, kprintf(k, "%s%s", base, suffix));
    if (custom || !multi || table) return;
    const char *others = "";
    int seq = first;
    for (size_t li = 0; li < ir->ui_lang_count && li < RP_UI_LANG_ACTIONS; ++li) {
        const char *code = ir->ui_langs[li].code, *text = rp_ui_text_for(ir, id, li);
        if (text == NULL || strcmp(text, base) == 0) continue;
        const char *act = kprintf(k, "%s_%s", name, code);
        const char *is = kprintf(k, "RPLANGUAGE = \"%s\"", code, NULL);
        s_(&pk->customaction, act); i_(&pk->customaction, 19); null_(&pk->customaction); s_(&pk->customaction, kprintf(k, "%s%s", text, suffix));
        const char *c = kprintf(k, "%s AND %s", found, is);
        s_(&pk->iexec, act); s_(&pk->iexec, c); i_(&pk->iexec, seq);
        s_(&pk->iui, act); s_(&pk->iui, c); i_(&pk->iui, seq);
        ++seq;
        others = *others ? kprintf(k, "%s OR %s", others, is) : is;
    }
    if (*others) *cond = kprintf(k, "%s AND NOT (%s)", found, others);
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
    // x47: [package] replaces - every version (0.0.0 and up) of those products is removed by
    // RemoveExistingProducts in this installation, as an older version of this one is.
    for (size_t i = 0; i < ir->replace_count; ++i) {
        s_(&pk->upgrade, ir->replaces[i]); s_(&pk->upgrade, "0.0.0"); null_(&pk->upgrade); null_(&pk->upgrade);
        char n[40];
        snprintf(n, sizeof n, "RP_REPLACED_%u", (unsigned)(i & 15));
        i_(&pk->upgrade, 0x100); null_(&pk->upgrade); s_(&pk->upgrade, kdup(k, n));
    }

    refusal(pk, "RP_RefuseDowngrade", "NewerInstalled", ir->downgrade_message, "", "RP_NEWER_FOUND", 32, &pk->newer_cond);
    // RFC-0003 section 9 (T1): an old version that must be removed by hand first. The removal command
    // names the found product ([RP_REFUSED_OLD]) and keeps Restart Manager off for that removal.
    if (ir->refuse_below) {
        refusal(pk, "RP_RefuseOld", "OlderRefused", ir->refuse_message, " msiexec /x [RP_REFUSED_OLD] /qn MSIRESTARTMANAGERCONTROL=Disable",
                "RP_REFUSED_OLD", 40, &pk->older_cond);
    }
}

// The install, UI, administrative and advertisement sequences, with the [action.*] do/undo pairs.
// A DLL of the Binary table (RFC-0031 B6): the file read and kept as its stream. reg_bad on failure.
static void add_binary(pkg_t *pk, const char *name, const char *source) {
    uint8_t *data = NULL;
    size_t len = 0;
    if (source == NULL || rp_pal_read_file(pk->alloc, source, 1u << 24, &data, &len) != PROVEN_OK) {
        pk->reg_bad = true;
        return;
    }
    s_(&pk->binary, name); b_(&pk->binary, data, len);
    if (pk->act_bin_count < 64) pk->act_bin[pk->act_bin_count++] = data;   // freed at the end
    else rp_mem_free(pk->alloc, data);
}

// The sequence number of a standard action (MS Learn "Suggested InstallExecuteSequence"), for
// before/after; 0 when the name is not one rubrapack places.
static int32_t standard_sequence(const char *name) {
    static const struct { const char *name; int32_t seq; } std[] = {
        { "InstallInitialize", 1500 }, { "ProcessComponents", 1600 }, { "UnpublishComponents", 1700 },
        { "RemoveFiles", 3500 }, { "InstallFiles", 4000 }, { "DuplicateFiles", 4210 }, { "MoveFiles", 3800 },
        { "WriteRegistryValues", 5000 }, { "RegisterProduct", 6100 }, { "PublishComponents", 6500 },
        { "InstallServices", 5800 }, { "StartServices", 5900 }, { "InstallODBC", 5400 }, { "RegisterFonts", 5300 },
        { "InstallFinalize", 6600 },
    };
    for (size_t i = 0; i < sizeof std / sizeof std[0]; ++i) {
        if (strcmp(std[i].name, name) == 0) return std[i].seq;
    }
    return 0;
}

// RFC-0031 B5/B6: one custom action as written - an installed exe, an installed DLL's function, or
// a DLL in the Binary table - in the chosen phase, placed before/after a standard action or at a
// sequence number.
static void lower_single_action(pkg_t *pk, const rp_ir_action_t *a) {
    const rp_ir_t *ir = pk->ir;
    keep_t *k = pk->k;
    int type, src_kind;         // CustomAction Type bits; Source: 1 Directory/property, 2 File, 3 Component, 6 Binary
    const char *source = NULL, *target = NULL;
    if (a->kind == RP_ACT_EXE) {
        const lfile_t *f = lfile_of(pk, a->run_file);
        if (f == NULL) { pk->reg_bad = true; return; }
        type = a->phase == RP_PHASE_IMMEDIATE ? 18 : 18 | 0x400;        // an installed exe (Source: File) + its arguments
        src_kind = 2;
        source = f->key;
        target = a->args ? a->args : "";
    } else if (a->kind == RP_ACT_DLL) {
        const lfile_t *f = lfile_of(pk, a->dll_file);
        if (f == NULL) { pk->reg_bad = true; return; }
        type = a->phase == RP_PHASE_IMMEDIATE ? 17 : 17 | 0x400;        // a DLL installed with the product
        src_kind = 2;
        source = f->key;
        target = a->entry;
    } else {                    // a DLL in the Binary table
        char bkey[40];
        rp_key_derive('B', a->id, bkey);
        add_binary(pk, kdup(k, bkey), a->binary_source);
        type = a->phase == RP_PHASE_IMMEDIATE ? 1 : 1 | 0x400;
        src_kind = 6;
        source = kdup(k, bkey);
        target = a->entry;
    }
    (void)src_kind;
    bool deferred = a->phase != RP_PHASE_IMMEDIATE;
    if (a->phase == RP_PHASE_COMMIT) type |= 0x200;
    if (a->phase == RP_PHASE_ROLLBACK) type |= 0x100;
    // Who it runs as (a deferred action only): the package default, or as asked.
    if (deferred) {
        bool as_system = a->as_user == 1 ? false : a->as_user == 0 ? ir->scope == 0 : ir->scope == 0;
        if (as_system) type |= 0x800;          // no impersonation: the SYSTEM account (per-machine only)
    }
    if (a->ignore_exit) type |= 0x40;
    const char *name = kprintf(k, "RP_Act_%s", a->id, NULL);
    s_(&pk->customaction, name); i_(&pk->customaction, type); s_(&pk->customaction, source); s_(&pk->customaction, target ? target : "");
    // A deferred action's data: CustomActionData is set by an immediate action just before it.
    if (deferred && a->data) {
        const char *setter = kprintf(k, "RP_ActData_%s", a->id, NULL);
        s_(&pk->customaction, setter); i_(&pk->customaction, 51); s_(&pk->customaction, name); s_(&pk->customaction, a->data);
    }
    // Placement in the execute sequence. before/after give the named action's number +/- 1; a bare
    // sequence number is used as given; the default is just before InstallFinalize for a deferred
    // action, or after InstallInitialize for an immediate one.
    int32_t seq = a->sequence;
    if (seq == 0 && (a->before || a->after)) {
        int32_t base = standard_sequence(a->before ? a->before : a->after);
        seq = base == 0 ? 0 : base + (a->after ? 1 : -1);
    }
    if (seq == 0) {
        // A default that does not collide across the package's own actions: each gets its own slot by
        // its index (rollback and immediate just after InstallInitialize, the rest late, past the
        // commits' NR twins at 6592..6596). lower_sequences still errors if two ever land together.
        size_t idx = 0;
        for (size_t j = 0; j < ir->action_count; ++j) if (&ir->actions[j] == a) idx = j;
        seq = a->phase == RP_PHASE_ROLLBACK ? 1502 + (int32_t)idx
            : a->phase == RP_PHASE_IMMEDIATE ? 1496 - (int32_t)idx
            : 6597 + (int32_t)idx;
    }
    if (deferred && a->data) { s_(&pk->iexec, kprintf(k, "RP_ActData_%s", a->id, NULL)); s_(&pk->iexec, a->when); i_(&pk->iexec, seq - 1); }
    s_(&pk->iexec, name); s_(&pk->iexec, a->when); i_(&pk->iexec, seq);
}

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
        const char *cond = exec[i].seq == 30 && pk->newer_cond ? pk->newer_cond : exec[i].cond;
        s_(&pk->iexec, exec[i].action); s_(&pk->iexec, cond); i_(&pk->iexec, exec[i].seq);
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
    // RFC-0031 wave 1: the standard actions of the tables written (MS Learn "Suggested InstallExecuteSequence").
    if (pk->movefile.filled) { s_(&pk->iexec, "MoveFiles"); null_(&pk->iexec); i_(&pk->iexec, 3800); }
    if (pk->odbcdriver.filled || pk->odbcsource.filled) {
        s_(&pk->iexec, "SetODBCFolders"); null_(&pk->iexec); i_(&pk->iexec, 1100);
        s_(&pk->iexec, "RemoveODBC"); null_(&pk->iexec); i_(&pk->iexec, 2400);
        s_(&pk->iexec, "InstallODBC"); null_(&pk->iexec); i_(&pk->iexec, 5400);
    }
    if (pk->publish.filled) {
        s_(&pk->iexec, "UnpublishComponents"); null_(&pk->iexec); i_(&pk->iexec, 1700);
        s_(&pk->iexec, "PublishComponents"); null_(&pk->iexec); i_(&pk->iexec, 6500);
    }
    if (pk->isolated.filled) {
        s_(&pk->iexec, "IsolateComponents"); null_(&pk->iexec); i_(&pk->iexec, 950);
        s_(&pk->iui, "IsolateComponents"); null_(&pk->iui); i_(&pk->iui, 950);
    }
    if (pk->report) {                   // RFC-0003 6: right after InstallInitialize, so it runs last in a rollback
        s_(&pk->iexec, "RP_ReportPrepare"); null_(&pk->iexec); i_(&pk->iexec, 1495);
        s_(&pk->iexec, "RP_Report"); null_(&pk->iexec); i_(&pk->iexec, 1502);
    }
    if (pk->preflight) {                // before InstallValidate (1400): nothing has been touched yet
        s_(&pk->iexec, "RP_Preflight"); null_(&pk->iexec); i_(&pk->iexec, 1390);
    }
    if (pk->cleanup) {                  // RFC-0026: last in the script; the commit runs at InstallFinalize
        s_(&pk->iexec, "RP_CleanupPrepare"); null_(&pk->iexec); i_(&pk->iexec, 6590);
        s_(&pk->iexec, "RP_CleanupRegister"); null_(&pk->iexec); i_(&pk->iexec, 6591);
        if (ir->scope == 0) {           // after InstallFiles (4000), before DuplicateFiles (4210)
            s_(&pk->iexec, "RP_RescuePrepare"); null_(&pk->iexec); i_(&pk->iexec, 4190);
            s_(&pk->iexec, "RP_RescueApplyRollback"); null_(&pk->iexec); i_(&pk->iexec, 4191);
            s_(&pk->iexec, "RP_RescueApply"); null_(&pk->iexec); i_(&pk->iexec, 4192);
        }
    }
    if (pk->any_rplan) {                // after the Undo actions (3401...), before RemoveFiles (3500)
        s_(&pk->iexec, "RP_RemovePrepare"); null_(&pk->iexec); i_(&pk->iexec, 3490);
        s_(&pk->iexec, "RP_RemoveApplyRollback"); null_(&pk->iexec); i_(&pk->iexec, 3491);
        s_(&pk->iexec, "RP_RemoveApply"); null_(&pk->iexec); i_(&pk->iexec, 3492);
        s_(&pk->iexec, "RP_RemoveCommit"); null_(&pk->iexec); i_(&pk->iexec, 3493);
    }
    if (pk->any_purge || pk->extra) {   // with them; the commits run at InstallFinalize, after RemoveFolders
        s_(&pk->iexec, "RP_PurgePrepare"); null_(&pk->iexec); i_(&pk->iexec, 3494);
        s_(&pk->iexec, "RP_PurgeUser"); null_(&pk->iexec); i_(&pk->iexec, 3495);
        if (ir->scope == 0) { s_(&pk->iexec, "RP_PurgeMachine"); null_(&pk->iexec); i_(&pk->iexec, 3496); }
        else if (ir->scope == 2) { s_(&pk->iexec, "RP_PurgeMachine"); s_(&pk->iexec, "ALLUSERS=1"); i_(&pk->iexec, 3496); }
        if (pk->any_others) { s_(&pk->iexec, "RP_PurgeOthers"); null_(&pk->iexec); i_(&pk->iexec, 3497); }
    }
    {                                   // RFC-0031 L2: the commit actions' twins, when rollback is disabled
        static const char *const commits[] = { "RP_CleanupRegister", "RP_RemoveCommit", "RP_PurgeUser", "RP_PurgeMachine", "RP_PurgeOthers" };
        const bool have[] = { pk->cleanup, pk->any_rplan, pk->any_purge || pk->extra, (pk->any_purge || pk->extra) && ir->scope != 1, pk->any_others };
        for (int i = 0; i < 5; ++i) {
            if (!have[i]) continue;
            const char *cond = i == 3 && ir->scope == 2 ? "RollbackDisabled AND ALLUSERS=1" : "RollbackDisabled";
            s_(&pk->iexec, kprintf(k, "%sNR", commits[i], NULL)); s_(&pk->iexec, cond); i_(&pk->iexec, 6592 + i);
        }
    }
    if (pk->menu) {
        for (int i = 1; i <= 4; ++i) {
            char set[32];
            snprintf(set, sizeof set, "RP_MenuData%d", i);
            s_(&pk->iexec, kdup(k, set)); null_(&pk->iexec); i_(&pk->iexec, 1013 + i);       // 1014..1017: each its own (ICE82)
        }
        s_(&pk->iexec, "RP_MenuRemoveRollback"); s_(&pk->iexec, "REMOVE = \"ALL\""); i_(&pk->iexec, 3479);
        s_(&pk->iexec, "RP_MenuRemove"); s_(&pk->iexec, "REMOVE = \"ALL\""); i_(&pk->iexec, 3480);
        s_(&pk->iexec, "RP_MenuRegisterRollback"); s_(&pk->iexec, "NOT Installed"); i_(&pk->iexec, 5899);
        s_(&pk->iexec, "RP_MenuRegister"); s_(&pk->iexec, "NOT (REMOVE = \"ALL\")"); i_(&pk->iexec, 5900);
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
        s_(&pk->iexec, "RP_RefuseOld"); s_(&pk->iexec, pk->older_cond); i_(&pk->iexec, 31);
        s_(&pk->iui, "RP_RefuseOld"); s_(&pk->iui, pk->older_cond); i_(&pk->iui, 31);
    }
    for (size_t i = 0; i < na; ++i) {
        const rp_ir_action_t *a = &ir->actions[i];
        if (a->kind != RP_ACT_PAIR) {           // RFC-0031 B5/B6: a single action as written
            lower_single_action(pk, a);
            continue;
        }
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
        const char *cond = ui_rows[i].seq == 30 && pk->newer_cond ? pk->newer_cond : ui_rows[i].cond;
        s_(&pk->iui, ui_rows[i].action); s_(&pk->iui, cond); i_(&pk->iui, ui_rows[i].seq);
    }
    if (pk->details) {                  // with the progress page up (1280), around ExecuteAction (1300)
        s_(&pk->iui, "RP_Details"); null_(&pk->iui); i_(&pk->iui, 1290);
        s_(&pk->iui, "RP_DetailsDone"); null_(&pk->iui); i_(&pk->iui, 1301);
    }
    if (pk->maintain) {                 // where the dialog sets show RpMaintenanceDlg; full UI only
        s_(&pk->iui, "RP_Maintain"); s_(&pk->iui, "Installed AND NOT RESUME AND NOT Preselected AND NOT PATCH AND UILevel = 5"); i_(&pk->iui, 1240);
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
    // RFC-0031 B5: no two execute-sequence rows share a number (an author's [action] sequence that
    // lands on rubrapack's own), and a deferred run/dll action is not sequenced before the files are
    // installed. The rows are Action, Condition, Sequence.
    const rows_t *seqs[] = { &pk->iexec, &pk->iui };
    for (size_t t = 0; t < 2 && err == PROVEN_OK; ++t) {
        const rows_t *q = seqs[t];
        for (size_t a = 0; a + 3 <= q->filled; a += 3) {
            if (q->cells[a + 2].kind != RP_MSI_INT) continue;
            for (size_t b = a + 3; b + 3 <= q->filled; b += 3) {
                if (q->cells[b + 2].kind == RP_MSI_INT && q->cells[b + 2].i == q->cells[a + 2].i) {
                    rp_srcdiag_add(pk->diags, (rp_pos_t){ 1, 1 }, "RP1313", false,
                                   "two actions share sequence %d (%.*s and %.*s); give an [action] a different sequence, before or after",
                                   (int)q->cells[a + 2].i, (int)q->cells[a].len, (const char *)q->cells[a].bytes,
                                   (int)q->cells[b].len, (const char *)q->cells[b].bytes);
                    err = PROVEN_ERR_INVALID_FORMAT;
                }
            }
        }
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
                const char *face = has_hangul(pk->lic, lic_len) ? "Malgun Gothic" : rtf_face(ir->ui_lang_count ? ir->ui_langs[0].font : NULL, "en");
                err = rp_ui_text_to_rtf(alloc, pk->lic, lic_len, face, &pk->rtf, &rtf_len);
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
            err = rp_ui_text_to_rtf(alloc, raw, raw_len, rtf_face(ir->ui_langs[li].font, ir->ui_langs[li].code), &pk->lang_rtf[li], &lang_rtf_len[li]);
            rp_mem_free(alloc, raw);
        }
    }
    if (err == PROVEN_OK && ir->banner_source) err = rp_pal_read_file(alloc, ir->banner_source, 1u << 22, &pk->banner, &banner_len);
    // RFC-0031 B10: read each bitmap dialog-control's .bmp into memory for the Binary table. The
    // bytes stay in pk->ctl_imgs (freed with the package), as the UI tables keep a pointer to them.
    for (size_t k = 0; err == PROVEN_OK && k < ir->dialog_control_count; ++k)
        pk->nctl_img += ir->dialog_controls[k].image_source != NULL;
    if (err == PROVEN_OK && pk->nctl_img) {
        pk->ctl_imgs = rp_mem_alloc(alloc, pk->nctl_img, sizeof *pk->ctl_imgs);
        if (pk->ctl_imgs == NULL) err = PROVEN_ERR_NOMEM;
        for (size_t k = 0, gi = 0; err == PROVEN_OK && k < ir->dialog_control_count; ++k) {
            const rp_ir_dialog_control_t *x = &ir->dialog_controls[k];
            if (x->image_source == NULL) continue;
            uint8_t *data = NULL;
            size_t dlen = 0;
            err = rp_pal_read_file(alloc, x->image_source, 1u << 22, &data, &dlen);
            pk->ctl_imgs[gi++] = (rp_ui_control_image_t){ x->id, data, dlen };
        }
    }
    if (err == PROVEN_OK) {
        rp_ui_input_t in = { dkey(ir, ir->ui_install_dir ? ir->ui_install_dir : "INSTALLDIR"), pk->rtf, rtf_len, pk->banner, banner_len,
                             { 0 }, { 0 }, pk->details, pk->any_purge, pk->ctl_imgs, pk->nctl_img };
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
        if (pk->ui->seqs[i].exec) {
            s_(&pk->iexec, pk->ui->seqs[i].action); s_(&pk->iexec, pk->ui->seqs[i].condition); i_(&pk->iexec, pk->ui->seqs[i].sequence);
        }
        // The finished, cancelled and failed pages end an administrative installation too (ICE20).
        if (pk->ui->seqs[i].sequence < 0) {
            s_(&pk->aui, pk->ui->seqs[i].action); s_(&pk->aui, pk->ui->seqs[i].condition); i_(&pk->aui, pk->ui->seqs[i].sequence);
        }
    }
    for (size_t i = 0; pk->ui && i < pk->ui->ca_count; ++i) {       // RFC-0012: set-property actions; RFC-0028: RpLangApply
        s_(&pk->customaction, pk->ui->cas[i].action); i_(&pk->customaction, pk->ui->cas[i].type); s_(&pk->customaction, pk->ui->cas[i].source);
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
                  .comp_attr = ir->arch == RP_ARCH_X86 ? 0 : 256, .qplan = "RPQ1", .rplan = "RPR1", .pplan = "RPP1" };
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
    rows_init(&pk->movefile, alloc, "MoveFile", movefile_cols, 7);
    rows_init(&pk->odbcdriver, alloc, "ODBCDriver", odbcdriver_cols, 5);
    rows_init(&pk->odbcattr, alloc, "ODBCAttribute", odbcattr_cols, 3);
    rows_init(&pk->odbcsource, alloc, "ODBCDataSource", odbcsource_cols, 5);
    rows_init(&pk->odbcsattr, alloc, "ODBCSourceAttribute", odbcsattr_cols, 3);
    rows_init(&pk->publish, alloc, "PublishComponent", publish_cols, 5);
    rows_init(&pk->isolated, alloc, "IsolatedComponent", isolated_cols, 2);
    // The P3 tables are written only when they have rows, so packages without them stay as they were.
    rows_t *all[] = { &pk->property, &pk->directory, &pk->component, &pk->feature, &pk->featurecomp, &pk->file,
                      &pk->filehash, &pk->media, &pk->upgrade, &pk->customaction, &pk->iexec, &pk->iui, &pk->createfolder,
                      &pk->aexec, &pk->aui, &pk->advt, &pk->registry, &pk->removereg, &pk->shortcut, &pk->removefile,
                      &pk->duplicate, &pk->environment, &pk->inifile, &pk->removeini, &pk->launch, &pk->appsearch,
                      &pk->reglocator, &pk->drlocator, &pk->signature, &pk->complocator, &pk->svcinstall, &pk->svccontrol,
                      &pk->font, &pk->lockperm, &pk->binary, &pk->icon, &pk->condition, &pk->movefile, &pk->odbcdriver,
                      &pk->odbcattr, &pk->odbcsource, &pk->odbcsattr, &pk->publish, &pk->isolated };
    const size_t always = 16;

    lower_properties(pk, product_code);
    lower_directories(pk);
    lower_features(pk);
    lower_files(pk);
    lower_registry(pk);
    lower_addon(pk);
    lower_shortcuts(pk);
    lower_removes(pk);
    lower_env(pk);
    lower_ini(pk);
    lower_services(pk);
    lower_helper_actions(pk);
    lower_permissions(pk);
    lower_more(pk);
    lower_fonts(pk);
    lower_searches(pk);
    lower_copies(pk);
    lower_folders(pk);
    proven_err_t err = ir->module ? PROVEN_OK : lower_cabinets(pk, cab_stem, limits, jobs);     // a module writes its own (rp_msm_write)
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
    if (err == PROVEN_OK && ir->module) {         // a merge module (RFC-0017)
        rp_msi_wtable_t tables[sizeof all / sizeof all[0]];
        size_t nt = 0;
        for (size_t i = 0; i < sizeof all / sizeof all[0]; ++i) tables[nt++] = all[i]->t;
        rp_cab_file_t *cf = pk->nfiles ? rp_mem_alloc(alloc, pk->nfiles, sizeof *cf) : NULL;
        if (pk->nfiles && cf == NULL) err = PROVEN_ERR_NOMEM;
        for (size_t i = 0; err == PROVEN_OK && i < pk->nfiles; ++i) cf[i] = (rp_cab_file_t){ pk->files[i].key, pk->files[i].data, pk->files[i].size };
        const char *why = NULL;
        if (err == PROVEN_OK) {
            err = rp_msm_write(alloc, ir, tables, nt, cf, pk->nfiles, jobs ? jobs : rp_pal_cpu_count(), limits, sink, out, len, &why);
            if (err != PROVEN_OK && err != PROVEN_ERR_NOMEM) {
                rp_srcdiag_add(diags, (rp_pos_t){ 1, 1 }, "RP1518", false, "the merge module %s", why ? why : "cannot be written");
                err = PROVEN_ERR_INVALID_FORMAT;
            }
        }
        rp_mem_free(alloc, cf);
    } else if (err == PROVEN_OK) {
        enum { MERGED_TABLES = 64 };     // tables merge modules may add (RFC-0016 3)
        rp_msi_wtable_t tables[sizeof all / sizeof all[0] + 9 + MERGED_TABLES];   // + ui tables + _Validation
        size_t nt = 0;
        for (size_t i = 0; i < sizeof all / sizeof all[0]; ++i) {
            // AppSearch looks every locator up in Signature, which must then exist even when empty
            // (error 2228 otherwise: RFC-0012 V5 on the VM for a folder; x69 for a registry value,
            // whose package then failed to install).
            bool need = all[i] == &pk->signature && pk->ir->search_count;
            if (i < always || all[i]->t.row_count > 0 || need) tables[nt++] = all[i]->t;
        }
        for (size_t t = 0; pk->ui && t < pk->ui->table_count; ++t) {
            if (strcmp(pk->ui->tables[t].name, "Binary") != 0) tables[nt++] = pk->ui->tables[t];
        }
        // Merge modules (RFC-0016 3): their rows join the tables, their cabinets become streams.
        rp_msi_module_t *mods = ir->merge_count ? rp_mem_alloc(alloc, ir->merge_count, sizeof *mods) : NULL;
        rp_msi_wstream_t *streams = pk->streams;
        size_t nstreams = ir->cab_external ? 0 : pk->nstreams;
        const rp_msi_wtable_t *extra[64];
        size_t nextra = 0;
        if (ir->merge_count && mods == NULL) err = PROVEN_ERR_NOMEM;
        if (err == PROVEN_OK && ir->merge_count) {
            streams = rp_mem_alloc(alloc, nstreams + ir->merge_count, sizeof *streams);
            if (streams == NULL) err = PROVEN_ERR_NOMEM;
            else if (nstreams) memcpy(streams, pk->streams, nstreams * sizeof *streams);
        }
        size_t nmods = 0;
        for (size_t k = 0; err == PROVEN_OK && k < ir->merge_count; ++k) {
            const rp_ir_merge_t *x = &ir->merges[k];
            const char *why = NULL;
            err = rp_msi_module_open(alloc, x->source, &mods[k], &why);
            nmods = k + 1;
            rp_msi_wstream_t cab = { 0 };
            if (err == PROVEN_OK) err = rp_msi_module_configure(&mods[k], (const char *const *)x->config, x->config_count, x->feature, &why);
            if (err == PROVEN_OK) {
                err = rp_msi_module_merge(&mods[k], tables, &nt, sizeof tables / sizeof tables[0] - 1, dkey(ir, x->dir), x->feature,
                                          (unsigned)k + 1, &cab, &why);
            }
            if (err != PROVEN_OK) {
                rp_srcdiag_add(diags, x->pos, "RP1517", false, "[merge.%s]: '%s' %s", x->id, x->shown ? x->shown : "",
                               why ? why : "cannot be merged");
                err = PROVEN_ERR_INVALID_FORMAT;
                break;
            }
            if (cab.name) streams[nstreams++] = cab;
            const rp_msi_wtable_t *v = rp_msi_module_validation(&mods[k]);
            if (v && nextra < sizeof extra / sizeof extra[0]) extra[nextra++] = v;
        }
        const char *missing = NULL;
        if (err == PROVEN_OK) err = rp_msi_validation(alloc, tables, nt, extra, nextra, &tables[nt], &missing);
        if (err == PROVEN_ERR_NOT_FOUND) {
            rp_srcdiag_add(diags, (rp_pos_t){ 1, 1 }, "RP2020", false, "internal: no _Validation rule for a column of %s", missing);
        }
        if (err == PROVEN_OK) {
            rp_msi_wdb_t db = { 65001, tables, nt + 1, summary, summary_len, streams, nstreams };
            err = rp_msi_lint(alloc, &db, diags);     // RFC-0001 7.1: build always checks what it writes
            if (err == PROVEN_OK) err = rp_msi_write_to(alloc, &db, 12, limits, sink, out, len);
            rp_mem_free(alloc, (void *)tables[nt].cells);
        }
        for (size_t k = 0; k < nmods; ++k) rp_msi_module_close(&mods[k]);
        rp_mem_free(alloc, mods);
        if (streams != pk->streams) rp_mem_free(alloc, streams);
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
    for (size_t g = 0; g < pk->nctl_img; ++g) rp_mem_free(alloc, (uint8_t *)pk->ctl_imgs[g].bmp);
    rp_mem_free(alloc, pk->ctl_imgs);
    for (size_t i = 0; i < pk->act_bin_count; ++i) rp_mem_free(alloc, pk->act_bin[i]);
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
        int64_t v = rp_int_or_zero(fb);
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
