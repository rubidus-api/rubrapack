// src/cli/new_from.c - `rubrapack new <file>.toml --from <package.msi> [--dist <new folder>]`
// (RFC-0023): a source made from an existing MSI, for moving a package to rubrapack.
//
// Contract: the files are extracted into the dist folder exactly as `rubrapack extract` lays them
// out, and the source written next to it describes the package so that `rubrapack build` makes one
// whose files, folders, features, registry values, shortcuts, environment variables, INI values,
// services, public properties, launch conditions, copies, removals, fonts and permissions match the
// original's; its product, upgrade and key-path component codes are carried over, so it upgrades the
// original. What a rubrapack source cannot say (custom actions, the original's own dialogs, searches,
// COM tables, ...) is listed, table by table with its row count, in a comment at the end of the
// source - nothing is dropped silently.
//
// Keys of the original become IDs where they are valid rubrapack IDs (letters, digits and '_', not
// starting with a digit, unused); others are renamed, and every reference to a renamed file or folder
// in a formatted value ([#Key], [!Key], [Key]) is rewritten.

#include "rubrapack/buf.h"
#include "rubrapack/cfb.h"
#include "rubrapack/diag.h"
#include "rubrapack/inspect.h"
#include "rubrapack/limits.h"
#include "rubrapack/mem.h"
#include "rubrapack/msi.h"
#include "rubrapack/pal.h"
#include "rubrapack/pe.h"
#include "rubrapack/suminfo.h"

#include "proven/heap.h"

#include "new_int.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

enum { MAX_INPUT = 1u << 30, MAX_IDS = 1u << 16 };

typedef struct {
    proven_allocator_t alloc;
    void             **owned;
    size_t             nowned, capowned;
    bool               nomem;
    const rp_msi_wdb_t *db;
    const rp_cfb_t     *cfb;
    int                arch;        // 0 x64, 1 x86, 2 arm64
    // IDs given out, and the renames of the original's keys.
    char             **ids;
    size_t             nids, capids;
    char             **ren_from, **ren_to;
    int               *ren_kind;    // 'd' directory, 'f' file
    size_t             nren, capren;
    rp_buf_t           notes;       // "# ..." lines for the end of the source
    const char        *subject;     // the summary's Subject (PID 3), or NULL
    const char        *publisher;   // --publisher: also write [msix] and [msix-app], or NULL
} cx_t;

// ---- small helpers -----------------------------------------------------------------------------

static void *own(cx_t *c, void *p) {
    if (p == NULL) {
        c->nomem = true;
        return NULL;
    }
    if (c->nowned == c->capowned) {
        size_t cap = c->capowned ? c->capowned * 2 : 256;
        void **v = rp_mem_alloc(c->alloc, cap, sizeof *v);
        if (v == NULL) {
            c->nomem = true;
            rp_mem_free(c->alloc, p);
            return NULL;
        }
        if (c->nowned) memcpy(v, c->owned, c->nowned * sizeof *v);
        rp_mem_free(c->alloc, c->owned);
        c->owned = v;
        c->capowned = cap;
    }
    c->owned[c->nowned++] = p;
    return p;
}

static char *dupn(cx_t *c, const char *s, size_t n) {
    char *d = own(c, rp_mem_alloc(c->alloc, n + 1, 1));
    if (d == NULL) return NULL;
    memcpy(d, s, n);
    d[n] = '\0';
    return d;
}

static char *dup(cx_t *c, const char *s) { return s ? dupn(c, s, strlen(s)) : NULL; }

static char *fmt2(cx_t *c, const char *f, const char *a, const char *b) {
    size_t n = strlen(f) + strlen(a) + (b ? strlen(b) : 0) + 1;
    char *d = own(c, rp_mem_alloc(c->alloc, n, 1));
    if (d) snprintf(d, n, f, a, b);
    return d;
}

typedef struct {
    const rp_msi_wtable_t *t;
} table_t;

static table_t table(const cx_t *c, const char *name) {
    for (size_t i = 0; i < c->db->table_count; ++i) {
        if (strcmp(c->db->tables[i].name, name) == 0) return (table_t){ &c->db->tables[i] };
    }
    return (table_t){ NULL };
}

static size_t rows(table_t t) { return t.t ? t.t->row_count : 0; }

static int col(table_t t, const char *name) {
    for (size_t k = 0; t.t && k < t.t->column_count; ++k) {
        if (strcmp(t.t->columns[k].name, name) == 0) return (int)k;
    }
    return -1;
}

// The cell as a string ("" for null), or NULL for a missing column.
static const char *str(cx_t *c, table_t t, size_t r, const char *name) {
    int k = col(t, name);
    if (k < 0) return NULL;
    const rp_msi_cell_t *v = &t.t->cells[r * t.t->column_count + (size_t)k];
    if (v->kind == RP_MSI_INT) {
        char num[16];
        snprintf(num, sizeof num, "%ld", (long)v->i);
        return dup(c, num);
    }
    if (v->kind != RP_MSI_STR) return dup(c, "");
    return dupn(c, (const char *)v->bytes, v->len);
}

static bool is_null(table_t t, size_t r, const char *name) {
    int k = col(t, name);
    return k < 0 || t.t->cells[r * t.t->column_count + (size_t)k].kind == RP_MSI_NULL;
}

static int32_t num(table_t t, size_t r, const char *name, int32_t dflt) {
    int k = col(t, name);
    if (k < 0) return dflt;
    const rp_msi_cell_t *v = &t.t->cells[r * t.t->column_count + (size_t)k];
    return v->kind == RP_MSI_INT ? v->i : dflt;
}

// The row whose first column is `key`, or SIZE_MAX.
static size_t find(cx_t *c, table_t t, const char *keycol, const char *key) {
    for (size_t r = 0; key && r < rows(t); ++r) {
        const char *k = str(c, t, r, keycol);
        if (k && strcmp(k, key) == 0) return r;
    }
    return SIZE_MAX;
}

static const char *property(cx_t *c, const char *name) {
    table_t p = table(c, "Property");
    size_t r = find(c, p, "Property", name);
    return r == SIZE_MAX ? NULL : str(c, p, r, "Value");
}

// "short|long" -> long; "target:source" -> target.
static const char *long_name(cx_t *c, const char *s, bool split) {
    const char *colon = split ? strchr(s, ':') : NULL;
    size_t n = colon ? (size_t)(colon - s) : strlen(s);
    const char *bar = memchr(s, '|', n);
    return bar ? dupn(c, bar + 1, n - (size_t)(bar + 1 - s)) : dupn(c, s, n);
}

static void note(cx_t *c, const char *f, const char *a, const char *b) {
    rp_buf_puts(&c->notes, "# ");
    char line[1024];
    snprintf(line, sizeof line, f, a ? a : "", b ? b : "");
    rp_buf_puts(&c->notes, line);
    rp_buf_byte(&c->notes, '\n');
}

// The MSI's standard folder properties as rubrapack's path bases, per architecture (64-bit, 32-bit
// package); NULL where rubrapack has none. Kept in step with standard_folder() in src/msi/lower.c.
static const char *standard_base(const char *key, int arch) {
    bool w64 = arch != 1;
    static const char *const map[][3] = {
        { "ProgramFiles64Folder", "ProgramFiles", NULL },
        { "ProgramFilesFolder", "ProgramFiles(x86)", "ProgramFiles" },
        { "CommonFiles64Folder", "CommonProgramFiles", NULL },
        { "CommonFilesFolder", NULL, "CommonProgramFiles" },
        { "AppDataFolder", "APPDATA", "APPDATA" },
        { "LocalAppDataFolder", "LOCALAPPDATA", "LOCALAPPDATA" },
        { "CommonAppDataFolder", "ProgramData", "ProgramData" },
        { "StartMenuFolder", "StartMenu", "StartMenu" },
        { "ProgramMenuFolder", "Programs", "Programs" },
        { "DesktopFolder", "Desktop", "Desktop" },
        { "StartupFolder", "Startup", "Startup" },
        { "WindowsFolder", "SystemRoot", "SystemRoot" },
        { "System64Folder", "System", NULL },
        { "SystemFolder", NULL, "System" },
        { "FontsFolder", "Fonts", "Fonts" },
        { "TempFolder", "TEMP", "TEMP" },
    };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; ++i) {
        if (strcmp(key, map[i][0]) == 0) return map[i][w64 ? 1 : 2];
    }
    return NULL;
}

static bool standard_key(const char *key) {
    static const char *const keys[] = { "ProgramFiles64Folder", "ProgramFilesFolder", "CommonFiles64Folder", "CommonFilesFolder",
                                        "AppDataFolder", "LocalAppDataFolder", "CommonAppDataFolder", "StartMenuFolder",
                                        "ProgramMenuFolder", "DesktopFolder", "StartupFolder", "WindowsFolder", "System64Folder",
                                        "SystemFolder", "System16Folder", "FontsFolder", "TempFolder", "PersonalFolder",
                                        "FavoritesFolder", "SendToFolder", "TemplateFolder", "NetHoodFolder", "PrintHoodFolder",
                                        "RecentFolder", "AdminToolsFolder", "MyPicturesFolder", "WindowsVolume", "TARGETDIR", NULL };
    for (size_t i = 0; keys[i]; ++i) {
        if (strcmp(key, keys[i]) == 0) return true;
    }
    return false;
}

// ---- IDs and renames ---------------------------------------------------------------------------

static bool id_used(const cx_t *c, const char *s) {
    for (size_t i = 0; i < c->nids; ++i) {
        if (strcmp(c->ids[i], s) == 0) return true;
    }
    return false;
}

static void remember(cx_t *c, const char *id) {
    if (c->nids == c->capids) {
        size_t cap = c->capids ? c->capids * 2 : 256;
        char **v = rp_mem_alloc(c->alloc, cap, sizeof *v);
        if (v == NULL) {
            c->nomem = true;
            return;
        }
        if (c->nids) memcpy(v, c->ids, c->nids * sizeof *v);
        rp_mem_free(c->alloc, c->ids);
        c->ids = v;
        c->capids = cap;
    }
    c->ids[c->nids++] = (char *)id;
}

static void add_rename(cx_t *c, const char *from, const char *to, int kind) {
    if (c->nren == c->capren) {
        size_t cap = c->capren ? c->capren * 2 : 64;
        char **f = rp_mem_alloc(c->alloc, cap, sizeof *f), **t = rp_mem_alloc(c->alloc, cap, sizeof *t);
        int *k = rp_mem_alloc(c->alloc, cap, sizeof *k);
        if (f == NULL || t == NULL || k == NULL) {
            rp_mem_free(c->alloc, f);
            rp_mem_free(c->alloc, t);
            rp_mem_free(c->alloc, k);
            c->nomem = true;
            return;
        }
        if (c->nren) {
            memcpy(f, c->ren_from, c->nren * sizeof *f);
            memcpy(t, c->ren_to, c->nren * sizeof *t);
            memcpy(k, c->ren_kind, c->nren * sizeof *k);
        }
        rp_mem_free(c->alloc, c->ren_from);
        rp_mem_free(c->alloc, c->ren_to);
        rp_mem_free(c->alloc, c->ren_kind);
        c->ren_from = f;
        c->ren_to = t;
        c->ren_kind = k;
        c->capren = cap;
    }
    c->ren_from[c->nren] = dup(c, from);
    c->ren_to[c->nren] = (char *)to;
    c->ren_kind[c->nren++] = kind;
}

// Names rubrapack keeps for itself: the standard folders, and the shape of its generated keys
// (C_, D_, F_ or R_ and 20 hex digits).
static bool reserved(const char *s) {
    if (standard_key(s) || strcmp(s, "SourceDir") == 0) return true;
    if (strlen(s) != 22 || (s[0] != 'C' && s[0] != 'D' && s[0] != 'F' && s[0] != 'R') || s[1] != '_') return false;
    for (size_t k = 2; k < 22; ++k) {
        if (!((s[k] >= '0' && s[k] <= '9') || (s[k] >= 'a' && s[k] <= 'f'))) return false;
    }
    return true;
}

// An ID for the original's key: the key itself when it is one, else a cleaned form, made unique.
// `kind` ('d', 'f' or 0) records a rename so formatted values can follow it.
static const char *make_id(cx_t *c, const char *key, int kind) {
    char base[72];
    size_t n = 0;
    for (const char *p = key; *p && n < 56; ++p) {
        bool ok = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_';
        base[n++] = ok ? *p : '_';
    }
    base[n] = '\0';
    char id[80];
    snprintf(id, sizeof id, "%s%s", n == 0 || (base[0] >= '0' && base[0] <= '9') || strncmp(base, "RP_", 3) == 0 || strncmp(base, "Rp", 2) == 0 ? "K_" : "", base);
    if (reserved(id)) snprintf(id, sizeof id, "K_%.60s", base);       // base is at most 56 characters
    char cand[96];
    snprintf(cand, sizeof cand, "%s", id);
    for (int k = 2; id_used(c, cand) && k < 100000; ++k) snprintf(cand, sizeof cand, "%s_%d", id, k);
    const char *out = dup(c, cand);
    if (out == NULL) return "";
    remember(c, out);
    if (kind && strcmp(out, key) != 0) add_rename(c, key, out, kind);
    return out;
}

static const char *renamed(const cx_t *c, const char *key, int kind) {
    for (size_t i = 0; i < c->nren; ++i) {
        if (c->ren_kind[i] == kind && strcmp(c->ren_from[i], key) == 0) return c->ren_to[i];
    }
    return key;
}

// A formatted value with [#File], [!File] and [Dir] references following the renames.
static const char *follow(cx_t *c, const char *v) {
    if (c->nren == 0 || strchr(v, '[') == NULL) return v;
    rp_buf_t b = rp_buf_new(c->alloc, 1u << 20);
    for (const char *p = v; *p;) {
        const char *close = *p == '[' ? strchr(p, ']') : NULL;
        if (close == NULL) {
            rp_buf_byte(&b, (uint8_t)*p++);
            continue;
        }
        const char *in = p + 1;
        int kind = *in == '#' || *in == '!' ? 'f' : 'd';
        const char *name = kind == 'f' ? in + 1 : in;
        char key[128];
        snprintf(key, sizeof key, "%.*s", (int)(close - name), name);
        rp_buf_byte(&b, '[');
        if (kind == 'f') rp_buf_byte(&b, (uint8_t)*in);
        rp_buf_puts(&b, renamed(c, key, kind));
        rp_buf_byte(&b, ']');
        p = close + 1;
    }
    const char *out = b.err == PROVEN_OK ? dupn(c, (const char *)b.data, b.len) : v;
    rp_buf_free(&b);
    return out;
}

// A formatted text that holds only literal characters, as rubrapack writes one ("[\\[]" for '['),
// back to the text.
static const char *unescape(cx_t *c, const char *v) {
    if (strstr(v, "[\\") == NULL) return v;
    char *out = dup(c, v);
    if (out == NULL) return v;
    size_t o = 0;
    for (const char *p = v; *p;) {
        if (p[0] == '[' && p[1] == '\\' && p[2] && p[3] == ']') {
            out[o++] = p[2];
            p += 4;
        } else {
            out[o++] = *p++;
        }
    }
    out[o] = '\0';
    return out;
}

// ---- writing TOML ------------------------------------------------------------------------------

// A TOML basic string; '$' doubled, since $(NAME) is rubrapack's substitution.
static void tstr(rp_buf_t *b, const char *s) {
    rp_buf_byte(b, '"');
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        if (*p == '"' || *p == '\\') {
            rp_buf_byte(b, '\\');
            rp_buf_byte(b, *p);
        } else if (*p == '\n') {
            rp_buf_puts(b, "\\n");
        } else if (*p == '\t') {
            rp_buf_puts(b, "\\t");
        } else if (*p < 0x20 || *p == 0x7F) {
            char u[8];
            snprintf(u, sizeof u, "\\u%04X", *p);
            rp_buf_puts(b, u);
        } else if (*p == '$') {
            rp_buf_puts(b, "$$");
        } else {
            rp_buf_byte(b, *p);
        }
    }
    rp_buf_byte(b, '"');
}

static void kv(rp_buf_t *b, const char *key, const char *v) {
    if (v == NULL) return;
    rp_buf_puts(b, key);
    rp_buf_puts(b, " = ");
    tstr(b, v);
    rp_buf_byte(b, '\n');
}

// The same for a value already in rubrapack's own syntax (a path with $(Base)): '$' left alone.
static void kv_raw(rp_buf_t *b, const char *key, const char *v) {
    rp_buf_puts(b, key);
    rp_buf_puts(b, " = \"");
    for (const char *p = v; *p; ++p) {
        if (*p == '"' || *p == '\\') rp_buf_byte(b, '\\');
        rp_buf_byte(b, (uint8_t)*p);
    }
    rp_buf_puts(b, "\"\n");
}

static void kb(rp_buf_t *b, const char *key, bool v) {
    rp_buf_puts(b, key);
    rp_buf_puts(b, v ? " = true\n" : " = false\n");
}

static void kn(rp_buf_t *b, const char *key, long v) {
    rp_buf_puts(b, key);
    rp_buf_puts(b, " = ");
    rp_buf_long(b, v);
    rp_buf_byte(b, '\n');
}

static void head(rp_buf_t *b, const char *kind, const char *id) {
    rp_buf_puts(b, "\n[");
    rp_buf_puts(b, kind);
    if (id) {
        rp_buf_byte(b, '.');
        rp_buf_puts(b, id);
    }
    rp_buf_puts(b, "]\n");
}

// ---- folders -----------------------------------------------------------------------------------

typedef struct {
    const char *key, *parent, *dd;
    const char *target;     // the path `extract` gives it (relative, '/' separated)
    const char *id;         // the [dir] ID, or NULL (a standard folder, the root, or not representable)
    const char *path;       // `path = ...` of its [dir]
    const char *alias;      // for "." folders: the folder it is the same as
    int         state;
} dir_t;

typedef struct {
    dir_t *d;
    size_t n;
} dirs_t;

static dir_t *dir_find(dirs_t *ds, const char *key) {
    for (size_t i = 0; key && i < ds->n; ++i) {
        if (strcmp(ds->d[i].key, key) == 0) return &ds->d[i];
    }
    return NULL;
}

static bool is_root(const dir_t *x) { return x->parent[0] == '\0' || strcmp(x->parent, x->key) == 0; }

// The extract layout (src/cli/extract.c resolve(): a standard folder right under the root keeps
// its key, "." is the parent) and the rubrapack path, in one walk.
static void dir_resolve(cx_t *c, dirs_t *ds, dir_t *x, unsigned depth) {
    if (x->state == 2 || depth > 64) return;
    if (x->state == 1) {
        x->state = 2;
        return;
    }
    x->state = 1;
    if (is_root(x)) {
        x->target = "";
        x->state = 2;
        return;
    }
    dir_t *p = dir_find(ds, x->parent);
    if (p == NULL) {
        x->state = 2;
        return;
    }
    dir_resolve(c, ds, p, depth + 1);
    const char *name = long_name(c, x->dd, true);
    bool dot = strcmp(name, ".") == 0;
    const char *pname = name;           // in a rubrapack path '$' is written "$$"
    if (strchr(name, '$')) {
        rp_buf_t e = rp_buf_new(c->alloc, 4096);
        for (const char *q = name; *q; ++q) rp_buf_puts(&e, *q == '$' ? "$$" : (char[2]){ *q, 0 });
        pname = e.err == PROVEN_OK ? dupn(c, (const char *)e.data, e.len) : name;
        rp_buf_free(&e);
    }
    x->target = dot ? (is_root(p) ? x->key : p->target) : (p->target[0] ? fmt2(c, "%s/%s", p->target, name) : dup(c, name));
    // Its rubrapack path: under a standard folder ($(Base)/name), under another [dir] ($(ID)/name).
    const char *base = standard_base(x->key, c->arch);
    if (standard_key(x->key)) {
        x->path = base ? fmt2(c, "$(%s)", base, NULL) : NULL;
    } else if (dot) {
        x->alias = p->id ? p->id : NULL;
        x->path = p->path;
    } else if (is_root(p)) {
        // A folder straight under the root: the root of a drive. rubrapack has no such base; it goes
        // under Program Files instead, said in a note.
        x->path = fmt2(c, "$(ProgramFiles)/%s", pname, NULL);
        note(c, "Directory '%s' was at the root of a drive (\\%s); here it is under Program Files.", x->key, name);
    } else if (p->id) {
        x->path = fmt2(c, "$(%s)/%s", p->id, pname);
    } else if (p->path) {
        x->path = fmt2(c, "%s/%s", p->path, pname);
    } else {
        note(c, "Directory '%s' is under '%s', a folder rubrapack has no path for; what it holds is left out.", x->key, x->parent);
    }
    x->state = 2;
}

// ---- the source ------------------------------------------------------------------------------

typedef struct {
    const char *key;        // Component
    const char *id_guid;    // ComponentId
    const char *dir;        // Directory_
    const char *keypath;    // KeyPath
    const char *cond;       // Condition, or NULL
    int32_t     attr;
    const char *feature;    // the first feature it belongs to (feature ID), or NULL
    const char *keyfile;    // the ID of its key file, or NULL
} comp_t;

static comp_t *comp_find(comp_t *v, size_t n, const char *key) {
    for (size_t i = 0; key && i < n; ++i) {
        if (strcmp(v[i].key, key) == 0) return &v[i];
    }
    return NULL;
}

static uint16_t arch_machine(int arch) { return arch == 1 ? 0x14C : arch == 2 ? 0xAA64 : 0x8664; }

// Reads a stream of the package (an icon) into dist, returning its source path, or NULL.
static const char *save_stream(cx_t *c, const char *table_name, const char *key, const char *dist, const char *dist_rel, const char *name) {
    char sname[160];
    snprintf(sname, sizeof sname, "%s.%s", table_name, key);
    uint16_t packed[32];
    size_t plen;
    uint32_t id;
    if (rp_msi_stream_name(sname, false, packed, &plen) != PROVEN_OK || rp_cfb_find(c->cfb, 0, packed, plen, &id) != PROVEN_OK ||
        c->cfb->entries[id].size > (64u << 20)) {
        return NULL;
    }
    size_t n = (size_t)c->cfb->entries[id].size;
    uint8_t *buf = own(c, rp_mem_alloc(c->alloc, n + 1, 1));
    if (buf == NULL || rp_cfb_read(c->cfb, id, buf, n) != PROVEN_OK) return NULL;
    if (n < 4 || buf[0] != 0 || buf[1] != 0 || buf[2] != 1 || buf[3] != 0) return NULL;      // an .ico only (not a program's icons)
    size_t nl = strlen(name);
    if (nl < 4 || (strcmp(name + nl - 4, ".ico") != 0 && strcmp(name + nl - 4, ".ICO") != 0)) name = fmt2(c, "%s.ico", name, NULL);
    char *folder = fmt2(c, "%s/_icons", dist, NULL);
    if (folder == NULL) return NULL;
    if (rp_pal_stat(c->alloc, folder, NULL) == RP_FS_NONE && rp_pal_mkdir_new(c->alloc, folder) != PROVEN_OK) return NULL;
    char *path = fmt2(c, "%s/%s", folder, name);
    if (path == NULL) return NULL;
    // The same icon named twice (the product's and a shortcut's) is written once.
    if (rp_pal_stat(c->alloc, path, NULL) == RP_FS_NONE && rp_pal_write_file_new(c->alloc, path, buf, n) != PROVEN_OK) return NULL;
    return fmt2(c, "%s/_icons/%s", dist_rel, name);
}

static const char *const handled_tables[] = {
    // carried over
    "Property", "Directory", "Component", "File", "Feature", "FeatureComponents", "Registry", "Shortcut", "Environment",
    "IniFile", "RemoveIniFile", "ServiceInstall", "ServiceControl", "LaunchCondition", "DuplicateFile", "RemoveFile", "Font",
    "MsiLockPermissionsEx", "Icon", "Upgrade", "Condition",
    // what rubrapack makes itself, or bookkeeping of the database
    "_Validation", "Media", "MsiFileHash", "InstallExecuteSequence", "InstallUISequence", "AdminExecuteSequence",
    "AdminUISequence", "AdvtExecuteSequence", "CreateFolder", "Binary", "Error", "ActionText", "UIText", "TextStyle",
    "_SummaryInformation", "Dialog", "Control", "ControlEvent", "ControlCondition", "EventMapping", "RadioButton", "ComboBox",
    "ListBox", "ListView", "CheckBox", "Billboard", "BBControl", "CustomAction", "MsiFileHash", NULL,
};

// The original's standard actions (and rubrapack's own); anything else in a sequence is a custom action.
static bool listed(const char *s, const char *const *list) {
    for (size_t i = 0; list[i]; ++i) {
        if (strcmp(s, list[i]) == 0) return true;
    }
    return false;
}

static int write_source(cx_t *c, rp_buf_t *b, const char *input, const char *dist, const char *dist_rel, const char *pkg_name) {
    // ---- [package]
    const char *name = property(c, "ProductName"), *maker = property(c, "Manufacturer"), *version = property(c, "ProductVersion");
    const char *upgrade = property(c, "UpgradeCode"), *product = property(c, "ProductCode"), *lang = property(c, "ProductLanguage");
    const char *allusers = property(c, "ALLUSERS"), *peruser = property(c, "MSIINSTALLPERUSER");
    rp_buf_puts(b, "# ");
    rp_buf_puts(b, pkg_name);
    rp_buf_puts(b, " - made by `rubrapack new --from` from an existing package; the files are in ");
    rp_buf_puts(b, dist_rel);
    rp_buf_puts(b, "/.\n# What could not be carried over is listed at the end.\n\nformat = 1\n");
    head(b, "package", NULL);
    kv(b, "name", name && name[0] ? name : "Unnamed product");
    if (c->subject && c->subject[0] && name && strcmp(c->subject, name) != 0) {
        bool ascii = true;
        for (const char *q = c->subject; *q; ++q) ascii &= (unsigned char)*q >= 0x20 && (unsigned char)*q < 0x7F;
        if (ascii) kv(b, "summary-name", c->subject);
    }
    kv(b, "manufacturer", maker && maker[0] ? maker : "Unknown");
    kv(b, "version", version && version[0] ? version : "1.0.0");
    kv(b, "arch", c->arch == 1 ? "x86" : c->arch == 2 ? "arm64" : "x64");
    if (upgrade && upgrade[0]) kv(b, "upgrade-code", upgrade);
    else note(c, "The package had no UpgradeCode; give [package] upgrade-code before building.", NULL, NULL);
    // The product code is not carried: each new version needs its own (rubrapack derives one), or
    // Windows takes the new package for the installed one (1638). Said in a note.
    if (product && product[0]) note(c, "The original's ProductCode was %s; this package gets its own, so raise the version to upgrade it.", product, NULL);
    if (lang && strcmp(lang, "1042") == 0) kv(b, "language", "ko-KR");
    else if (lang && strcmp(lang, "1033") != 0) note(c, "ProductLanguage %s became en-US (rubrapack writes en-US and ko-KR packages).", lang, NULL);
    // Scope: ALLUSERS=1 per machine; ALLUSERS=2 with MSIINSTALLPERUSER=1 per user, or dual when no
    // launch condition holds it to per-user.
    bool user_only = false;
    table_t lc = table(c, "LaunchCondition");
    for (size_t r = 0; r < rows(lc); ++r) user_only |= strcmp(str(c, lc, r, "Condition"), "Installed OR MSIINSTALLPERUSER = 1") == 0;
    bool per_user = allusers && strcmp(allusers, "2") == 0 && peruser && strcmp(peruser, "1") == 0;
    if (per_user) kv(b, "scope", user_only ? "user" : "dual");
    else if (!allusers || strcmp(allusers, "1") != 0) note(c, "ALLUSERS was '%s'; the package installs per machine here (scope).", allusers ? allusers : "", NULL);
    table_t dlg = table(c, "Dialog");
    if (rows(dlg)) {
        // The nearest of rubrapack's dialog sets, by what the original's pages were for.
        bool features = false, installdir = false, welcome = false;
        for (size_t r = 0; r < rows(dlg); ++r) {
            const char *dn = str(c, dlg, r, "Dialog");
            features |= strstr(dn, "Customize") || strstr(dn, "Feature");
            installdir |= strstr(dn, "InstallDir") || strstr(dn, "Browse");
            welcome |= strstr(dn, "Welcome") != NULL;
        }
        const char *set = features ? "features" : installdir ? "installdir" : welcome ? "minimal" : "basic";
        kv(b, "ui", set);
        bool own_set = find(c, dlg, "Dialog", "RpExitDlg") != SIZE_MAX;
        if (!own_set) note(c, "The original's own dialogs are replaced by rubrapack's \"%s\" set (ui).", set, NULL);
    }
    table_t ca0 = table(c, "CustomAction");
    size_t dg = find(c, ca0, "Action", "RP_RefuseDowngrade");
    if (dg != SIZE_MAX) {
        const char *m = str(c, ca0, dg, "Target");
        if (strcmp(m, "The same or a newer version of [ProductName] is already installed.") != 0 &&
            strcmp(m, "\xeb\x8d\x94 \xec\x83\x88 \xed\x8c\x90\xec\x9d\xb4\xeb\x82\x98 \xea\xb0\x99\xec\x9d\x80 \xed\x8c\x90\xec\x9d\x98 [ProductName]\xec\x9d\xb4(\xea\xb0\x80) \xec\x9d\xb4\xeb\xaf\xb8 \xec\x84\xa4\xec\xb9\x98\xeb\x90\x98\xec\x96\xb4 \xec\x9e\x88\xec\x8a\xb5\xeb\x8b\x88\xeb\x8b\xa4.") != 0) {
            kv(b, "downgrade-message", m);
        }
    }
    const char *reboot = property(c, "REBOOT");
    if (reboot && (reboot[0] == 'R' || reboot[0] == 'r')) kv(b, "reboot", "suppress");

    // ---- [arp]
    const char *nomod = property(c, "ARPNOMODIFY"), *norep = property(c, "ARPNOREPAIR"), *help = property(c, "ARPHELPLINK"),
               *about = property(c, "ARPURLINFOABOUT"), *icon = property(c, "ARPPRODUCTICON");
    const char *arp_icon = icon && icon[0] ? save_stream(c, "Icon", icon, dist, dist_rel, icon) : NULL;
    if ((nomod && nomod[0]) || (norep && norep[0]) || (help && help[0]) || (about && about[0]) || arp_icon) {
        head(b, "arp", NULL);
        if (nomod && nomod[0]) kb(b, "no-modify", true);
        if (norep && norep[0]) kb(b, "no-repair", true);
        if (help && help[0]) kv(b, "help", help);
        if (about && about[0]) kv(b, "about", about);
        if (arp_icon) kv(b, "icon", arp_icon);
    }

    // ---- folders
    table_t dt = table(c, "Directory");
    dirs_t ds = { own(c, rp_mem_alloc(c->alloc, rows(dt) + 1, sizeof(dir_t))), rows(dt) };
    if (ds.d == NULL) return RP_EXIT_IO;
    for (size_t r = 0; r < ds.n; ++r) {
        ds.d[r] = (dir_t){ str(c, dt, r, "Directory"), str(c, dt, r, "Directory_Parent"), str(c, dt, r, "DefaultDir"), NULL, NULL, NULL, NULL, 0 };
    }
    // Which folders something names: a component's, a shortcut's, an INI file's, a removal's, a
    // copy's, a working folder, a permission's - and the parent of an empty folder ([folder]).
    // A folder rubrapack itself made for a path (D_ and 20 hex digits) needs no [dir] of its own
    // when nothing names it; an empty folder is a [folder] in its parent, not a [dir].
    table_t ct_ = table(c, "Component"), ft_ = table(c, "File"), rt_ = table(c, "Registry"), cf_ = table(c, "CreateFolder");
    bool *named = own(c, rp_mem_alloc(c->alloc, ds.n + 1, sizeof *named));
    bool *empty = own(c, rp_mem_alloc(c->alloc, ds.n + 1, sizeof *empty));
    if (named == NULL || empty == NULL) return RP_EXIT_IO;
    memset(named, 0, (ds.n + 1) * sizeof *named);
    memset(empty, 0, (ds.n + 1) * sizeof *empty);
    for (size_t r = 0; r < rows(ct_); ++r) {
        const char *ck = str(c, ct_, r, "Component"), *cd = str(c, ct_, r, "Directory_");
        bool files = false, values = false, folder = false;
        for (size_t f = 0; f < rows(ft_) && !files; ++f) files = strcmp(str(c, ft_, f, "Component_"), ck) == 0;
        for (size_t f = 0; f < rows(rt_) && !values; ++f) values = strcmp(str(c, rt_, f, "Component_"), ck) == 0;
        for (size_t f = 0; f < rows(cf_) && !folder; ++f) folder = strcmp(str(c, cf_, f, "Component_"), ck) == 0;
        dir_t *d = dir_find(&ds, cd);
        if (d == NULL) continue;
        if (folder && !files && !values) {
            empty[d - ds.d] = true;
            dir_t *pd = dir_find(&ds, d->parent);
            if (pd) named[pd - ds.d] = true;
        } else {
            named[d - ds.d] = true;
        }
    }
    static const char *const refs[][2] = { { "Shortcut", "Directory_" }, { "Shortcut", "WkDir" }, { "IniFile", "DirProperty" },
                                           { "RemoveIniFile", "DirProperty" }, { "RemoveFile", "DirProperty" },
                                           { "DuplicateFile", "DestFolder" }, { "MsiLockPermissionsEx", "LockObject" },
                                           { "Control", "Property" } };
    for (size_t k = 0; k < sizeof refs / sizeof refs[0]; ++k) {
        table_t t = table(c, refs[k][0]);
        for (size_t r = 0; r < rows(t); ++r) {
            dir_t *d = dir_find(&ds, str(c, t, r, refs[k][1]));
            if (d) named[d - ds.d] = true;
        }
    }
    // IDs first (in table order), so references to renamed folders can be followed.
    for (size_t r = 0; r < ds.n; ++r) {
        dir_t *x = &ds.d[r];
        if (is_root(x) || standard_key(x->key)) continue;
        const char *nm = long_name(c, x->dd, true);
        if (strcmp(nm, ".") == 0) continue;
        bool generated = strlen(x->key) == 22 && x->key[0] == 'D' && x->key[1] == '_';
        if ((generated && !named[r]) || (empty[r] && !named[r])) continue;
        x->id = make_id(c, x->key, 'd');
    }
    for (size_t r = 0; r < ds.n; ++r) dir_resolve(c, &ds, &ds.d[r], 0);
    // A standard folder that holds files itself (fonts in Fonts, a tool in System) gets a [dir]
    // whose path is its base; its name in formatted values stays the standard property.
    {
        table_t ct0 = table(c, "Component");
        for (size_t r = 0; r < rows(ct0); ++r) {
            dir_t *x = dir_find(&ds, str(c, ct0, r, "Directory_"));
            if (x == NULL || x->id || !standard_key(x->key) || x->path == NULL) continue;
            const char *base = standard_base(x->key, c->arch);
            char clean[64];
            size_t n = 0;
            for (const char *q = base; *q && n < 60; ++q) {
                if ((*q >= 'A' && *q <= 'Z') || (*q >= 'a' && *q <= 'z') || (*q >= '0' && *q <= '9')) clean[n++] = *q;
            }
            clean[n] = '\0';
            x->id = make_id(c, fmt2(c, "%sDir", clean, NULL), 0);
        }
    }
    for (size_t r = 0; r < ds.n; ++r) {
        dir_t *x = &ds.d[r];
        if (x->alias) add_rename(c, x->key, x->alias, 'd');     // a "." folder is its parent: references follow
        if (x->id == NULL || x->path == NULL) continue;
        head(b, "dir", x->id);
        kv_raw(b, "path", x->path);
        const char *guard = property(c, "RP_GUARD");       // rubrapack's install folder guard
        if (guard) {
            size_t kl = strlen(x->key);
            for (const char *g = strstr(guard, x->key); g; g = strstr(g + 1, x->key)) {
                if ((g == guard || g[-1] == ';') && (g[kl] == '\0' || g[kl] == ';')) {
                    kb(b, "guard", true);
                    break;
                }
            }
        }
    }

    // ---- components and features
    table_t ct = table(c, "Component"), ft = table(c, "File"), fct = table(c, "FeatureComponents"), fe = table(c, "Feature");
    size_t ncomp = rows(ct);
    comp_t *comps = own(c, rp_mem_alloc(c->alloc, ncomp + 1, sizeof *comps));
    if (comps == NULL) return RP_EXIT_IO;
    for (size_t r = 0; r < ncomp; ++r) {
        comps[r] = (comp_t){ str(c, ct, r, "Component"), str(c, ct, r, "ComponentId"), str(c, ct, r, "Directory_"),
                             str(c, ct, r, "KeyPath"), is_null(ct, r, "Condition") ? NULL : str(c, ct, r, "Condition"),
                             num(ct, r, "Attributes", 0), NULL, NULL };
    }
    // Features: their IDs, then the components' first feature.
    const char **feat_ids = own(c, rp_mem_alloc(c->alloc, rows(fe) + 1, sizeof *feat_ids));
    if (feat_ids == NULL) return RP_EXIT_IO;
    for (size_t r = 0; r < rows(fe); ++r) feat_ids[r] = make_id(c, str(c, fe, r, "Feature"), 0);
    // One feature as rubrapack makes it when a source names none (Main, the product's name, hidden)
    // is left to that default.
    bool features_out = rows(fe) > 0;
    if (rows(fe) == 1 && strcmp(str(c, fe, 0, "Feature"), "Main") == 0 && name && strcmp(str(c, fe, 0, "Title"), name) == 0 &&
        num(fe, 0, "Display", 1) == 0 && num(fe, 0, "Level", 1) == 1 && is_null(fe, 0, "Feature_Parent")) {
        features_out = false;
    }
    for (size_t r = 0; r < rows(fct); ++r) {
        comp_t *k = comp_find(comps, ncomp, str(c, fct, r, "Component_"));
        size_t fr = find(c, fe, "Feature", str(c, fct, r, "Feature_"));
        if (k && k->feature == NULL && fr != SIZE_MAX) k->feature = feat_ids[fr];
    }
    table_t cond = table(c, "Condition");
    // In the order the original shows them (Display; 0, hidden, last), which is the order rubrapack
    // numbers them in.
    size_t *order = own(c, rp_mem_alloc(c->alloc, rows(fe) + 1, sizeof *order));
    if (order == NULL) return RP_EXIT_IO;
    for (size_t r = 0; r < rows(fe); ++r) order[r] = r;
    for (size_t i = 1; i < rows(fe); ++i) {
        for (size_t j = i; j > 0; --j) {
            int32_t a = num(fe, order[j - 1], "Display", 0), d = num(fe, order[j], "Display", 0);
            unsigned ua = a == 0 ? 0x7FFFFFFFu : (unsigned)(a < 0 ? -a : a), ud = d == 0 ? 0x7FFFFFFFu : (unsigned)(d < 0 ? -d : d);
            if (ua <= ud) break;
            size_t t = order[j];
            order[j] = order[j - 1];
            order[j - 1] = t;
        }
    }
    for (size_t oi = 0; oi < rows(fe) && features_out; ++oi) {
        size_t r = order[oi];
        head(b, "feature", feat_ids[r]);
        const char *title = str(c, fe, r, "Title"), *desc = str(c, fe, r, "Description"), *parent = str(c, fe, r, "Feature_Parent");
        kv(b, "title", title && title[0] ? title : str(c, fe, r, "Feature"));
        if (desc && desc[0]) kv(b, "description", desc);
        int32_t level = num(fe, r, "Level", 1), display = num(fe, r, "Display", 1), attr = num(fe, r, "Attributes", 0);
        if (parent && parent[0]) {
            size_t pr = find(c, fe, "Feature", parent);
            if (pr != SIZE_MAX) kv(b, "parent", feat_ids[pr]);
        }
        if (level >= 1 && level != 1) kn(b, "level", level);
        if (display == 0) kb(b, "hidden", true);
        if (attr & 0x10) kb(b, "required", true);
        if (attr & 0x2) kb(b, "follow-parent", true);
        // Level 0: never installed. A Condition row that sets level 0 keeps it out while it holds.
        const char *off = level == 0 ? "0" : NULL;
        for (size_t k = 0; k < rows(cond); ++k) {
            if (strcmp(str(c, cond, k, "Feature_"), str(c, fe, r, "Feature")) != 0) continue;
            const char *cc = str(c, cond, k, "Condition");
            size_t cl = strlen(cc);
            if (num(cond, k, "Level", 1) == 0 && off == NULL) {
                // rubrapack's own form for `when` ("NOT Installed AND NOT (X)") goes back to X.
                if (strncmp(cc, "NOT Installed AND NOT (", 23) == 0 && cl > 24 && cc[cl - 1] == ')') off = dupn(c, cc + 23, cl - 24);
                else off = fmt2(c, "NOT (%s)", cc, NULL);
                continue;
            }
            note(c, "A Condition row of feature '%s' sets another level (%s); left out.", str(c, fe, r, "Feature"), cc);
        }
        if (off) kv(b, "when", off);
    }

    // ---- files
    size_t nf = rows(ft);
    for (size_t r = 0; r < nf; ++r) {
        const char *key = str(c, ft, r, "File");
        const char *id = make_id(c, key, 'f');
        comp_t *k = comp_find(comps, ncomp, str(c, ft, r, "Component_"));
        if (k && k->keypath && strcmp(k->keypath, key) == 0) k->keyfile = id;
    }
    for (size_t r = 0; r < nf; ++r) {
        const char *key = str(c, ft, r, "File");
        const char *id = renamed(c, key, 'f');
        comp_t *k = comp_find(comps, ncomp, str(c, ft, r, "Component_"));
        dir_t *d = k ? dir_find(&ds, k->dir) : NULL;
        if (d && d->alias) d = dir_find(&ds, d->parent);
        const char *fname = long_name(c, str(c, ft, r, "FileName"), false);
        if (k == NULL || d == NULL || d->id == NULL || d->path == NULL) {
            note(c, "File '%s' is in a folder rubrapack cannot name; left out (%s).", key, fname);
            continue;
        }
        head(b, "file", id);
        kv(b, "dir", d->id);
        const char *src = d->target[0] ? fmt2(c, "%s/%s", d->target, fname) : fname;
        kv(b, "source", fmt2(c, "%s/%s", dist_rel, src));
        int32_t fattr = num(ft, r, "Attributes", 0);
        if (!(fattr & 0x200)) kb(b, "vital", false);
        if (k->keyfile && strcmp(k->keyfile, id) == 0 && k->id_guid && k->id_guid[0]) kv(b, "component-guid", k->id_guid);
        if (k->attr & 0x10) kb(b, "keep", true);
        if (k->feature && features_out) kv(b, "feature", k->feature);
        if (k->cond && k->cond[0]) kv(b, "when", k->cond);
        // A program or library of another architecture than the package's says so (any-arch).
        char *path = fmt2(c, "%s/%s", dist, src);
        uint8_t *data = NULL;
        size_t len = 0;
        if (path && rp_pal_read_file(c->alloc, path, 1u << 30, &data, &len) == PROVEN_OK) {
            rp_pe_info_t pe;
            if (rp_pe_read(data, len, &pe) == PROVEN_OK && pe.is_pe && pe.machine != arch_machine(c->arch)) kb(b, "any-arch", true);
            rp_mem_free(c->alloc, data);
        }
    }

    // ---- empty folders
    for (size_t r = 0; r < ncomp; ++r) {
        dir_t *d = dir_find(&ds, comps[r].dir);
        if (d == NULL || !empty[d - ds.d] || d->id) continue;
        dir_t *pd = dir_find(&ds, d->parent);
        if (pd == NULL || pd->id == NULL) {
            note(c, "The empty folder '%s' is in a folder rubrapack cannot name; left out.", d->key, NULL);
            continue;
        }
        head(b, "folder", make_id(c, d->key, 'd'));
        kv(b, "dir", pd->id);
        kv(b, "name", long_name(c, d->dd, true));
        if (comps[r].attr & 0x10) kb(b, "keep", true);
        if (comps[r].feature && features_out) kv(b, "feature", comps[r].feature);
    }

    // ---- registry
    table_t rt = table(c, "Registry"), st = table(c, "Shortcut");
    static const char *const roots[] = { "HKCR", "HKCU", "HKLM" };
    for (size_t r = 0; r < rows(rt); ++r) {
        const char *key = str(c, rt, r, "Registry"), *rk = str(c, rt, r, "Key"), *rn = str(c, rt, r, "Name");
        int32_t root = num(rt, r, "Root", 2);
        comp_t *k = comp_find(comps, ncomp, str(c, rt, r, "Component_"));
        bool null_value = is_null(rt, r, "Value");
        const char *v = null_value ? NULL : str(c, rt, r, "Value");
        if (null_value && (rn[0] == '\0' || strcmp(rn, "+") == 0 || strcmp(rn, "-") == 0 || strcmp(rn, "*") == 0)) {
            note(c, "Registry '%s' only creates or removes the key %s; left out.", key, rk);
            continue;
        }
        if (root < -1 || root > 2) {
            note(c, "Registry '%s' is under HKEY_USERS; left out.", key, NULL);
            continue;
        }
        // A shortcut's component keeps a value as its key path; rubrapack makes that itself.
        bool shortcut_comp = false;
        for (size_t s2 = 0; k && s2 < rows(st); ++s2) shortcut_comp |= strcmp(str(c, st, s2, "Component_"), k->key) == 0;
        if (shortcut_comp && k->keypath && strcmp(k->keypath, key) == 0) continue;
        if (root == 1 && !per_user) {
            note(c, "Registry '%s' writes HKCU in a per-machine package (it would be the installing user's); left out.", key, NULL);
            continue;
        }
        const char *id = make_id(c, key, 0);
        head(b, "registry", id);
        kv(b, "root", root == -1 ? "HKMU" : roots[root]);
        kv(b, "key", follow(c, rk));
        if (rn[0]) kv(b, "name", follow(c, rn));
        if (v == NULL) v = "";
        // MSI value prefixes: #x binary, #% expandable, # integer, ## a literal '#', [~] multi-string.
        if (strncmp(v, "#x", 2) == 0 || strncmp(v, "#X", 2) == 0) {
            kv(b, "type", "binary");
            kv(b, "value", v + 2);
        } else if (strncmp(v, "#%", 2) == 0) {
            kv(b, "type", "expand");
            kv(b, "value", follow(c, v + 2));
        } else if (strncmp(v, "##", 2) == 0) {
            kv(b, "value", follow(c, v + 1));
        } else if (v[0] == '#') {
            kv(b, "type", "dword");
            if (v[1] == '-') {          // MSI takes a signed integer; rubrapack the same bits unsigned
                char u[16];
                long long sv = 0;
                for (const char *q = v + 2; *q >= '0' && *q <= '9'; ++q) sv = sv * 10 + (*q - '0');
                snprintf(u, sizeof u, "%lu", (unsigned long)(uint32_t)(int32_t)-sv);
                rp_buf_puts(b, "value = ");
                rp_buf_puts(b, u);
                rp_buf_byte(b, '\n');
            } else {
                rp_buf_puts(b, "value = ");     // a TOML integer
                for (const char *q = v + (v[1] == '+' ? 2 : 1); *q >= '0' && *q <= '9'; ++q) rp_buf_byte(b, (uint8_t)*q);
                rp_buf_byte(b, '\n');
            }
        } else if (strstr(v, "[~]")) {
            kv(b, "type", "multi");
            rp_buf_puts(b, "value = [");
            bool first = true;
            for (const char *p = v; *p;) {
                const char *e = strstr(p, "[~]");
                size_t n = e ? (size_t)(e - p) : strlen(p);
                if (n) {
                    if (!first) rp_buf_puts(b, ", ");
                    tstr(b, follow(c, dupn(c, p, n)));
                    first = false;
                }
                p += n + (e ? 3 : 0);
            }
            rp_buf_puts(b, "]\n");
        } else {
            kv(b, "value", follow(c, v));
        }
        if (k && c->arch != 1 && !(k->attr & 0x100)) kv(b, "view", "32");
        if (k && k->keyfile) kv(b, "with", fmt2(c, "file:%s", k->keyfile, NULL));
        else {
            if (k && k->feature && features_out) kv(b, "feature", k->feature);
            if (k && k->cond && k->cond[0]) kv(b, "when", k->cond);
        }
    }

    // ---- shortcuts
    for (size_t r = 0; r < rows(st); ++r) {
        const char *key = str(c, st, r, "Shortcut"), *sd = str(c, st, r, "Directory_"), *target = str(c, st, r, "Target");
        comp_t *k = comp_find(comps, ncomp, str(c, st, r, "Component_"));
        const char *dir = strcmp(sd, "ProgramMenuFolder") == 0 ? "Programs" : strcmp(sd, "DesktopFolder") == 0 ? "Desktop"
                        : strcmp(sd, "StartMenuFolder") == 0   ? "StartMenu" : strcmp(sd, "StartupFolder") == 0 ? "Startup" : NULL;
        if (dir == NULL) {
            dir_t *d = dir_find(&ds, sd);
            if (d && d->alias) d = dir_find(&ds, d->parent);
            dir = d ? d->id : NULL;
        }
        const char *tfile = NULL;
        if (target[0] == '[' && (target[1] == '#' || target[1] == '!')) {
            const char *close = strchr(target, ']');
            if (close && close[1] == '\0') tfile = renamed(c, dupn(c, target + 2, (size_t)(close - target - 2)), 'f');
        } else if (target[0] == '[') {
            // "[DIR]name": the file of that name in that folder.
            const char *close = strchr(target, ']');
            if (close && close[1] && !strchr(close + 1, '[') && !strchr(close + 1, '\\')) {
                const char *dkey = dupn(c, target + 1, (size_t)(close - target - 1));
                for (size_t f = 0; f < nf && tfile == NULL; ++f) {
                    comp_t *fk = comp_find(comps, ncomp, str(c, ft, f, "Component_"));
                    if (fk == NULL) continue;
                    dir_t *fd = dir_find(&ds, fk->dir), *td = dir_find(&ds, dkey);
                    if (fd && td && (fd == td || (fd->alias && dir_find(&ds, fd->parent) == td) || (td->alias && dir_find(&ds, td->parent) == fd)) &&
                        strcmp(long_name(c, str(c, ft, f, "FileName"), false), close + 1) == 0) {
                        tfile = renamed(c, str(c, ft, f, "File"), 'f');
                    }
                }
            }
        } else if (k && k->keyfile) {
            tfile = k->keyfile;         // an advertised shortcut: the component's key file
        }
        if (dir == NULL || tfile == NULL) {
            note(c, "Shortcut '%s' (target %s) has no folder or file rubrapack can name; left out.", key, target);
            continue;
        }
        head(b, "shortcut", make_id(c, key, 0));
        kv(b, "dir", dir);
        kv(b, "name", long_name(c, str(c, st, r, "Name"), false));
        kv(b, "target", fmt2(c, "file:%s", tfile, NULL));
        const char *args = str(c, st, r, "Arguments"), *desc = str(c, st, r, "Description"), *wk = str(c, st, r, "WkDir"), *ic = str(c, st, r, "Icon_");
        if (args && args[0]) kv(b, "args", follow(c, args));
        if (desc && desc[0]) kv(b, "description", desc);
        if (wk && wk[0]) {
            dir_t *w = dir_find(&ds, wk);
            if (w && w->id) kv(b, "working-dir", w->id);
        }
        if (ic && ic[0]) {
            const char *p = save_stream(c, "Icon", ic, dist, dist_rel, ic);
            if (p) kv(b, "icon", p);
        }
        if (k && k->cond && k->cond[0]) kv(b, "when", k->cond);
    }

    // ---- environment
    table_t et = table(c, "Environment");
    for (size_t r = 0; r < rows(et); ++r) {
        const char *key = str(c, et, r, "Environment"), *n = str(c, et, r, "Name"), *v = is_null(et, r, "Value") ? NULL : str(c, et, r, "Value");
        bool remove_on_install = false, keep = true;
        while (*n == '=' || *n == '+' || *n == '-' || *n == '!' || *n == '*') {
            if (*n == '!') remove_on_install = true;
            if (*n == '-') keep = false;
            ++n;
        }
        if (remove_on_install || v == NULL) {
            note(c, "Environment '%s' removes %s at installation; left out.", key, n);
            continue;
        }
        const char *mode = NULL;
        if (strncmp(v, "[~];", 4) == 0) {
            mode = "append";
            v += 4;
        } else if (strlen(v) > 4 && strcmp(v + strlen(v) - 4, ";[~]") == 0) {
            mode = "prepend";
            v = dupn(c, v, strlen(v) - 4);
        }
        comp_t *k = comp_find(comps, ncomp, str(c, et, r, "Component_"));
        head(b, "env", make_id(c, key, 0));
        kv(b, "name", n);
        kv(b, "value", follow(c, v));
        if (mode) kv(b, "mode", mode);
        if (keep || (k && (k->attr & 0x10))) kb(b, "keep", true);     // no '-': the variable stays at removal
        if (k && k->feature && features_out) kv(b, "feature", k->feature);
        if (k && k->cond && k->cond[0]) kv(b, "when", k->cond);
    }

    // ---- INI files
    table_t it = table(c, "IniFile"), rit = table(c, "RemoveIniFile");
    for (int pass = 0; pass < 2; ++pass) {
        table_t t = pass ? rit : it;
        for (size_t r = 0; r < rows(t); ++r) {
            const char *key = str(c, t, r, pass ? "RemoveIniFile" : "IniFile"), *dp = str(c, t, r, "DirProperty");
            dir_t *d = dp[0] ? dir_find(&ds, dp) : NULL;
            if (d && d->alias) d = dir_find(&ds, d->parent);
            if (d == NULL || d->id == NULL) {
                note(c, "IniFile '%s' is in a folder rubrapack cannot name (%s); left out.", key, dp[0] ? dp : "the Windows folder");
                continue;
            }
            int32_t action = num(t, r, "Action", 0);
            comp_t *k = comp_find(comps, ncomp, str(c, t, r, "Component_"));
            head(b, "ini", make_id(c, key, 0));
            kv(b, "dir", d->id);
            kv(b, "file", long_name(c, str(c, t, r, "FileName"), false));
            kv(b, "section", str(c, t, r, "Section"));
            kv(b, "key", str(c, t, r, "Key"));
            const char *v = is_null(t, r, "Value") ? NULL : str(c, t, r, "Value");
            if (v) kv(b, "value", follow(c, v));
            if (pass) kv(b, "mode", "remove");
            else if (action == 3) kv(b, "mode", "add");
            if (k && k->feature && features_out) kv(b, "feature", k->feature);
            if (k && k->cond && k->cond[0]) kv(b, "when", k->cond);
        }
    }

    // ---- services
    table_t sv = table(c, "ServiceInstall"), sc = table(c, "ServiceControl");
    for (size_t r = 0; r < rows(sv); ++r) {
        const char *key = str(c, sv, r, "ServiceInstall"), *sname = str(c, sv, r, "Name");
        comp_t *k = comp_find(comps, ncomp, str(c, sv, r, "Component_"));
        if (k == NULL || k->keyfile == NULL) {
            note(c, "Service '%s' has no program as its component's key file; left out.", sname, NULL);
            continue;
        }
        head(b, "service", make_id(c, key, 0));
        kv(b, "file", fmt2(c, "file:%s", k->keyfile, NULL));
        kv(b, "name", sname);
        const char *dn = str(c, sv, r, "DisplayName"), *desc = str(c, sv, r, "Description"), *acct = str(c, sv, r, "StartName"),
                   *args = str(c, sv, r, "Arguments");
        if (dn && dn[0]) kv(b, "display-name", unescape(c, dn));
        if (desc && desc[0]) kv(b, "description", unescape(c, desc));
        int32_t start = num(sv, r, "StartType", 3);
        kv(b, "start", start == 2 ? "auto" : start == 4 ? "disabled" : "demand");
        if (acct && acct[0]) {
            const char *a = strstr(acct, "LocalService") ? "LocalService" : strstr(acct, "NetworkService") ? "NetworkService"
                          : strcmp(acct, "LocalSystem") == 0 ? "LocalSystem" : NULL;
            if (a) kv(b, "account", a);
            else note(c, "Service '%s' ran as '%s'; here as LocalSystem.", sname, acct);
        }
        if (args && args[0]) kv(b, "args", follow(c, args));
        for (size_t s = 0; s < rows(sc); ++s) {
            if (strcmp(str(c, sc, s, "Name"), sname) == 0 && (num(sc, s, "Event", 0) & 1)) kb(b, "start-on-install", true);
        }
    }

    // ---- rubrapack's own "start the program" box on the finished page (RP_Launch)
    // and the folder the install folder page lets the user choose (its PathEdit's property).
    table_t cat = table(c, "CustomAction"), ctl = table(c, "Control");
    const char *install_dir = NULL, *launch = NULL, *launch_args = NULL;
    for (size_t r = 0; r < rows(ctl) && rows(table(c, "Dialog")); ++r) {
        const char *pr = str(c, ctl, r, "Property");
        if (strcmp(str(c, ctl, r, "Type"), "PathEdit") != 0 || pr[0] == '_' || strcmp(pr, "INSTALLDIR") == 0) continue;
        dir_t *d = dir_find(&ds, pr);
        if (d && d->id && install_dir == NULL) install_dir = d->id;
    }
    size_t lr = find(c, cat, "Action", "RP_Launch");
    if (lr != SIZE_MAX && rows(table(c, "Dialog"))) {
        const char *tg = str(c, cat, lr, "Target");
        if (strncmp(tg, "\"[#", 3) == 0) {
            const char *close = strstr(tg + 3, "]\"");
            if (close) {
                launch = fmt2(c, "file:%s", renamed(c, dupn(c, tg + 3, (size_t)(close - tg - 3)), 'f'), NULL);
                if (close[2] == ' ' && close[3]) launch_args = follow(c, close + 3);
            }
        }
    }
    bool no_log = rows(table(c, "Dialog")) && find(c, table(c, "Dialog"), "Dialog", "RpExitDlg") != SIZE_MAX && property(c, "MsiLogging") == NULL;
    if (install_dir || launch || no_log) {
        head(b, "ui", NULL);
        if (no_log) kb(b, "save-log", false);
        if (install_dir) kv(b, "install-dir", install_dir);
        if (launch) {
            kv(b, "launch", launch);
            if (launch_args) kv(b, "launch-args", launch_args);
            const char *checked = property(c, "RPLAUNCH");
            if (!(checked && strcmp(checked, "1") == 0)) kb(b, "launch-checked", false);
        }
    }

    // ---- rubrapack's own [action] pairs (RP_<ID>_Do / RP_<ID>_Undo: a program of the package)
    for (size_t r = 0; r < rows(cat); ++r) {
        const char *an = str(c, cat, r, "Action");
        size_t al = strlen(an);
        if (strncmp(an, "RP_", 3) != 0 || al < 7 || strcmp(an + al - 3, "_Do") != 0 || (num(cat, r, "Type", 0) & 0x3F) != 18) continue;
        const char *aid = dupn(c, an + 3, al - 6);
        head(b, "action", make_id(c, aid, 0));
        kv(b, "run", fmt2(c, "file:%s", renamed(c, str(c, cat, r, "Source"), 'f'), NULL));
        kv(b, "do", str(c, cat, r, "Target"));
        size_t ur = find(c, cat, "Action", fmt2(c, "RP_%s_Undo", aid, NULL));
        if (ur != SIZE_MAX) kv(b, "undo", str(c, cat, ur, "Target"));
    }

    // ---- rubrapack's qword values (RP_QWORDS, read by its helper DLL): "RPQ1", then records of
    // root, view, key, name, value (16 hex digits), component, keep; each field "<length>:" and
    // that many UTF-16 units.
    const char *qp = property(c, "RP_QWORDS");
    if (qp && strncmp(qp, "RPQ1", 4) == 0) {
        const char *p = qp + 4;
        int nq = 0;
        while (*p) {
            const char *f[7];
            bool ok = true;
            for (int k = 0; k < 7 && ok; ++k) {
                long n = 0;
                while (*p >= '0' && *p <= '9') n = n * 10 + (*p++ - '0');
                if (*p++ != ':') {
                    ok = false;
                    break;
                }
                const char *st0 = p;
                for (long u = 0; u < n && *p; ++u) {     // n UTF-16 units of UTF-8 text
                    unsigned char ch = (unsigned char)*p;
                    size_t len = ch < 0x80 ? 1 : ch < 0xE0 ? 2 : ch < 0xF0 ? 3 : 4;
                    if (len == 4) ++u;
                    p += len;
                }
                f[k] = dupn(c, st0, (size_t)(p - st0));
            }
            if (!ok) break;
            char qid[32];
            snprintf(qid, sizeof qid, "Qword%d", ++nq);
            comp_t *k = comp_find(comps, ncomp, f[5]);
            head(b, "registry", make_id(c, qid, 0));
            kv(b, "root", f[0]);
            kv(b, "key", follow(c, f[2]));
            if (f[3][0]) kv(b, "name", follow(c, f[3]));
            kv(b, "type", "qword");
            kv(b, "value", fmt2(c, "0x%s", f[4], NULL));
            if (strcmp(f[1], "32") == 0 && c->arch != 1) kv(b, "view", "32");
            if (strcmp(f[6], "1") == 0) kb(b, "keep", true);
            if (k && k->keyfile) kv(b, "with", fmt2(c, "file:%s", k->keyfile, NULL));
            else if (k && k->feature && features_out) kv(b, "feature", k->feature);
        }
    }

    // ---- --publisher: the MSIX tables, so the same source builds an .msix
    if (c->publisher) {
        // Identity name: "Manufacturer.Product" in letters, digits, '.' and '-' (3 to 50).
        char ident[64];
        size_t n = 0;
        const char *parts[2] = { maker && maker[0] ? maker : "Unknown", name && name[0] ? name : "Product" };
        for (int k = 0; k < 2; ++k) {
            if (k && n < 49) ident[n++] = '.';
            for (const char *q = parts[k]; *q && n < 49; ++q) {
                if ((*q >= 'A' && *q <= 'Z') || (*q >= 'a' && *q <= 'z') || (*q >= '0' && *q <= '9') || *q == '-') ident[n++] = *q;
            }
        }
        ident[n] = '\0';
        if (n < 3) snprintf(ident, sizeof ident, "Converted.Package");
        head(b, "msix", NULL);
        kv(b, "identity-name", ident);
        kv(b, "publisher", c->publisher);
        // The application: what the Start menu shortcut starts, else the first program.
        const char *app = NULL;
        for (size_t r = 0; r < rows(st) && app == NULL; ++r) {
            const char *sd = str(c, st, r, "Directory_");
            if (strcmp(sd, "ProgramMenuFolder") != 0 && strcmp(sd, "StartMenuFolder") != 0) {
                dir_t *d = dir_find(&ds, sd);
                bool under = false;
                for (; d && !is_root(d); d = dir_find(&ds, d->parent)) under |= strcmp(d->key, "ProgramMenuFolder") == 0;
                if (!under) continue;
            }
            const char *tg = str(c, st, r, "Target");
            comp_t *k = comp_find(comps, ncomp, str(c, st, r, "Component_"));
            if (tg[0] == '[' && (tg[1] == '#' || tg[1] == '!')) {
                const char *close = strchr(tg, ']');
                if (close) app = renamed(c, dupn(c, tg + 2, (size_t)(close - tg - 2)), 'f');
            } else if (k && k->keyfile) {
                app = k->keyfile;
            }
        }
        for (size_t r = 0; r < nf && app == NULL; ++r) {
            const char *fnm = long_name(c, str(c, ft, r, "FileName"), false);
            size_t l = strlen(fnm);
            if (l > 4 && strcasecmp(fnm + l - 4, ".exe") == 0) app = renamed(c, str(c, ft, r, "File"), 'f');
        }
        if (app) {
            head(b, "msix-app", "App");
            kv(b, "executable", app);
            kv(b, "display-name", name && name[0] ? name : ident);
        } else {
            note(c, "The package has no program for an MSIX application; add an [msix-app.ID] before building an .msix.", NULL, NULL);
        }
    }

    // ---- public properties
    table_t pt = table(c, "Property");
    static const char *const own_props[] = { "ProductName", "Manufacturer", "ProductVersion", "ProductCode", "UpgradeCode",
                                             "ProductLanguage", "ALLUSERS", "MSIINSTALLPERUSER", "ARPNOMODIFY", "ARPNOREPAIR",
                                             "ARPHELPLINK", "ARPURLINFOABOUT", "ARPPRODUCTICON", "SecureCustomProperties",
                                             "DefaultUIFont", "ErrorDialog", "INSTALLLEVEL", "REINSTALLMODE", "ARPSYSTEMCOMPONENT", "REBOOT", NULL };
    const char *secure = property(c, "SecureCustomProperties");
    size_t private_props = 0;
    for (size_t r = 0; r < rows(pt); ++r) {
        const char *pn = str(c, pt, r, "Property");
        if (listed(pn, own_props) || strncmp(pn, "Msi", 3) == 0 || strncmp(pn, "MSI", 3) == 0 || strncmp(pn, "ARP", 3) == 0 ||
            strncmp(pn, "Wix", 3) == 0 || strncmp(pn, "WIX", 3) == 0 || strncmp(pn, "RP", 2) == 0 || strncmp(pn, "Rp", 2) == 0) {
            continue;
        }
        bool upper = true;
        for (const char *p = pn; *p; ++p) upper &= !(*p >= 'a' && *p <= 'z');
        if (!upper) {
            ++private_props;
            continue;
        }
        head(b, "property", make_id(c, pn, 0));
        kv(b, "value", follow(c, str(c, pt, r, "Value")));
        if (secure) {
            size_t n = strlen(pn);
            for (const char *p = strstr(secure, pn); p; p = strstr(p + 1, pn)) {
                if ((p == secure || p[-1] == ';') && (p[n] == '\0' || p[n] == ';')) {
                    kb(b, "secure", true);
                    break;
                }
            }
        }
    }
    if (private_props) {
        char n[24];
        snprintf(n, sizeof n, "%zu", private_props);
        note(c, "%s private properties (mixed case names) are left out; they belong to the original's own UI and actions.", n, NULL);
    }

    // ---- launch conditions
    for (size_t r = 0; r < rows(lc); ++r) {
        const char *cnd = str(c, lc, r, "Condition");
        if (strcmp(cnd, "Installed OR MSIINSTALLPERUSER = 1") == 0) continue;      // rubrapack's scope = "user"
        char idn[32];
        snprintf(idn, sizeof idn, "Require%zu", r + 1);
        head(b, "require", make_id(c, idn, 0));
        kv(b, "condition", cnd);
        kv(b, "message", str(c, lc, r, "Description"));
    }

    // ---- copies, removals, fonts, permissions
    table_t dup_t = table(c, "DuplicateFile");
    for (size_t r = 0; r < rows(dup_t); ++r) {
        const char *key = str(c, dup_t, r, "FileKey"), *src = renamed(c, str(c, dup_t, r, "File_"), 'f'), *df = str(c, dup_t, r, "DestFolder");
        comp_t *k = comp_find(comps, ncomp, str(c, dup_t, r, "Component_"));
        dir_t *d = dir_find(&ds, df && df[0] ? df : (k ? k->dir : NULL));
        if (d && d->alias) d = dir_find(&ds, d->parent);
        if (d == NULL || d->id == NULL) {
            note(c, "DuplicateFile '%s' goes to a folder rubrapack cannot name; left out.", key, NULL);
            continue;
        }
        head(b, "copy", make_id(c, key, 0));
        kv(b, "source", fmt2(c, "file:%s", src, NULL));
        kv(b, "dir", d->id);
        const char *dn = str(c, dup_t, r, "DestName");
        if (dn && dn[0]) kv(b, "name", long_name(c, dn, false));
    }
    table_t rf = table(c, "RemoveFile");
    for (size_t r = 0; r < rows(rf); ++r) {
        const char *key = str(c, rf, r, "FileKey"), *dp = str(c, rf, r, "DirProperty");
        dir_t *d = dir_find(&ds, dp);
        if (d && d->alias) d = dir_find(&ds, d->parent);
        if (d == NULL || d->id == NULL) {
            note(c, "RemoveFile '%s' names a folder rubrapack cannot name (%s); left out.", key, dp);
            continue;
        }
        int32_t mode = num(rf, r, "InstallMode", 2);
        // A shortcut's folder (and the folders above it, up to a standard one) is removed with it:
        // rubrapack writes that row itself.
        if (is_null(rf, r, "FileName") && mode == 2) {
            bool shortcut_folder = false;
            for (size_t s2 = 0; s2 < rows(st) && !shortcut_folder; ++s2) {
                for (dir_t *a = dir_find(&ds, str(c, st, s2, "Directory_")); a && !is_root(a) && !standard_key(a->key); a = dir_find(&ds, a->parent)) {
                    if (a == d || (d->alias == NULL && strcmp(a->key, dp) == 0)) shortcut_folder = true;
                }
            }
            if (shortcut_folder) continue;
        }
        head(b, "remove", make_id(c, key, 0));
        kv(b, "dir", d->id);
        if (!is_null(rf, r, "FileName")) kv(b, "name", long_name(c, str(c, rf, r, "FileName"), false));
        kv(b, "on", mode == 1 ? "install" : mode == 3 ? "both" : "uninstall");
        comp_t *k = comp_find(comps, ncomp, str(c, rf, r, "Component_"));
        if (k && k->feature && features_out) kv(b, "feature", k->feature);
    }
    table_t fo = table(c, "Font");
    for (size_t r = 0; r < rows(fo); ++r) {
        const char *fk = str(c, fo, r, "File_");
        head(b, "font", make_id(c, fmt2(c, "Font_%s", fk, NULL), 0));
        kv(b, "file", fmt2(c, "file:%s", renamed(c, fk, 'f'), NULL));
        const char *title = str(c, fo, r, "FontTitle");
        if (title && title[0]) kv(b, "title", title);
    }
    table_t lp = table(c, "MsiLockPermissionsEx");
    for (size_t r = 0; r < rows(lp); ++r) {
        const char *key = str(c, lp, r, "MsiLockPermissionsEx"), *obj = str(c, lp, r, "LockObject"), *tn = str(c, lp, r, "Table");
        const char *target = NULL;
        if (strcmp(tn, "File") == 0) target = fmt2(c, "file:%s", renamed(c, obj, 'f'), NULL);
        else if (strcmp(tn, "Directory") == 0 || strcmp(tn, "CreateFolder") == 0) {
            dir_t *d = dir_find(&ds, obj);
            if (d && d->id) target = fmt2(c, "dir:%s", d->id, NULL);
        } else if (strcmp(tn, "Registry") == 0) {
            target = fmt2(c, "registry:%s", obj, NULL);
        }
        if (target == NULL || !is_null(lp, r, "Condition")) {
            note(c, "MsiLockPermissionsEx '%s' (%s) is left out.", key, tn);
            continue;
        }
        head(b, "permission", make_id(c, key, 0));
        kv(b, "target", target);
        kv(b, "sddl", str(c, lp, r, col(lp, "SDDLText") >= 0 ? "SDDLText" : "SDDL"));
    }

    // ---- what was not carried over
    static const char *const std_actions[] = {
        "AppSearch", "LaunchConditions", "ValidateProductID", "CostInitialize", "FileCost", "CostFinalize", "InstallValidate",
        "InstallInitialize", "ProcessComponents", "UnpublishFeatures", "RemoveRegistryValues", "RemoveShortcuts", "RemoveFiles",
        "InstallFiles", "CreateShortcuts", "WriteRegistryValues", "RegisterUser", "RegisterProduct", "PublishFeatures",
        "PublishProduct", "InstallFinalize", "RemoveExistingProducts", "FindRelatedProducts", "MigrateFeatureStates",
        "CreateFolders", "RemoveFolders", "WriteEnvironmentStrings", "RemoveEnvironmentStrings", "WriteIniValues",
        "RemoveIniValues", "InstallServices", "StartServices", "StopServices", "DeleteServices", "DuplicateFiles",
        "RemoveDuplicateFiles", "RegisterFonts", "UnregisterFonts", "MsiPublishAssemblies", "MsiUnpublishAssemblies",
        "PatchFiles", "MoveFiles", "SelfRegModules", "SelfUnregModules", "BindImage", "AllocateRegistrySpace", "CCPSearch",
        "RMCCPSearch", "ExecuteAction", "ScheduleReboot", "ForceReboot", "ResolveSource", "InstallSFPCatalogFile",
        "SetODBCFolders", "InstallODBC", "RemoveODBC", "RegisterClassInfo", "UnregisterClassInfo", "RegisterProgIdInfo",
        "UnregisterProgIdInfo", "RegisterExtensionInfo", "UnregisterExtensionInfo", "RegisterMIMEInfo", "UnregisterMIMEInfo",
        "RegisterTypeLibraries", "UnregisterTypeLibraries", "RegisterComPlus", "UnregisterComPlus", "PublishComponents",
        "UnpublishComponents", "IsolateComponents", "SetODBCFolders", "MsiConfigureServices", "DisableRollback",
        "InstallAdminPackage", "ExecuteAction", "WelcomeDlg", "ProgressDlg", "ExitDialog", "FatalError", "UserExit",
        "PrepareDlg", "MaintenanceWelcomeDlg", "ResumeDlg", "ValidateProductID", NULL };
    size_t ncustom = 0;
    table_t ca = table(c, "CustomAction");
    for (size_t r = 0; r < rows(ca); ++r) ncustom += strncmp(str(c, ca, r, "Action"), "RP_", 3) != 0 && strncmp(str(c, ca, r, "Action"), "Rp", 2) != 0;     // not rubrapack's own
    rp_buf_t rest = rp_buf_new(c->alloc, 1u << 20);
    if (ncustom) {
        char n[24];
        snprintf(n, sizeof n, "%zu", ncustom);
        rp_buf_puts(&rest, "#   CustomAction: ");
        rp_buf_puts(&rest, n);
        rp_buf_puts(&rest, " (");
        size_t shown = 0;
        for (size_t r = 0; r < rows(ca) && shown < 12; ++r) {
            const char *an = str(c, ca, r, "Action");
            if (strncmp(an, "RP_", 3) == 0 || strncmp(an, "Rp", 2) == 0) continue;
            if (shown++) rp_buf_puts(&rest, ", ");
            rp_buf_puts(&rest, an);
        }
        if (ncustom > 12) rp_buf_puts(&rest, ", ...");
        rp_buf_puts(&rest, ") - what they did is not in this source; a program of the package run at\n"
                           "#     installation and removal can be an [action.ID] (run, do, undo)\n");
    }
    (void)std_actions;
    for (size_t i = 0; i < c->db->table_count; ++i) {
        const rp_msi_wtable_t *t = &c->db->tables[i];
        if (t->row_count == 0 || listed(t->name, handled_tables)) continue;
        char line[160];
        snprintf(line, sizeof line, "#   %s: %zu rows\n", t->name, t->row_count);
        rp_buf_puts(&rest, line);
    }
    table_t up = table(c, "Upgrade");
    for (size_t r = 0; r < rows(up); ++r) {
        const char *uc = str(c, up, r, "UpgradeCode");
        if (upgrade && strcasecmp(uc, upgrade) != 0) note(c, "Upgrade row for another product (%s) is left out.", uc, NULL);
    }
    for (size_t r = 0; r < ncomp; ++r) {
        bool has_shortcut = false;
        for (size_t s2 = 0; s2 < rows(st); ++s2) has_shortcut |= strcmp(str(c, st, s2, "Component_"), comps[r].key) == 0;
        if (!has_shortcut && comps[r].keyfile == NULL && comps[r].keypath && comps[r].keypath[0] && find(c, rt, "Registry", comps[r].keypath) != SIZE_MAX) {
            note(c, "Component '%s' had a registry key path; its values get a component of their own here.", comps[r].key, NULL);
        }
    }
    rp_buf_puts(b, "\n# ---- not carried over from ");
    rp_buf_puts(b, input);
    rp_buf_puts(b, " ----\n");
    if (rest.len == 0 && c->notes.len == 0) rp_buf_puts(b, "# (nothing)\n");
    if (rest.len) {
        rp_buf_puts(b, "# Tables rubrapack does not write from a source:\n");
        rp_buf_put(b, rest.data, rest.len);
    }
    if (c->notes.len) rp_buf_put(b, c->notes.data, c->notes.len);
    rp_buf_free(&rest);
    return b->err == PROVEN_OK && !c->nomem ? RP_EXIT_OK : RP_EXIT_IO;
}

// ---- the command -------------------------------------------------------------------------------

static const uint16_t summary_name[] = { 5, 'S', 'u', 'm', 'm', 'a', 'r', 'y', 'I', 'n', 'f', 'o', 'r', 'm', 'a', 't', 'i', 'o', 'n' };

int rpn_new_from(const char *source, const char *input, const char *dist, const char *publisher) {
    proven_allocator_t heap = proven_heap_allocator();
    if (rp_pal_stat(heap, source, NULL) != RP_FS_NONE) {
        rp_diag_error(RP_DIAG_OUTPUT, "'%s' exists already", source);
        return RP_EXIT_IO;
    }
    size_t il = strlen(input);
    if (il >= 4 && (strcmp(input + il - 4, ".msm") == 0 || strcmp(input + il - 4, ".MSM") == 0)) {
        rp_diag_error(RP_DIAG_EXTRA_ARGUMENT, "'%s' is a merge module: merge it with [merge.ID] instead", input);
        return RP_EXIT_USAGE;
    }
    // The files first, laid out as `extract` lays them out (and checked as it checks them).
    char *xargv[] = { "rubrapack", "extract", (char *)input, "-d", (char *)dist };
    int rc = rp_cmd_extract(5, xargv);
    if (rc != RP_EXIT_OK) return rc;

    uint8_t *data = NULL;
    size_t len = 0;
    if (rp_pal_read_file(heap, input, MAX_INPUT, &data, &len) != PROVEN_OK) {
        rp_diag_error(RP_DIAG_INPUT, "cannot read '%s'", input);
        return RP_EXIT_IO;
    }
    rp_limits_t lim = rp_limits_default();
    rp_cfb_t cfb;
    rp_msi_t msi;
    rp_msi_view_t view;
    const char *why = NULL;
    rc = RP_EXIT_IO;
    if (rp_cfb_open(&cfb, heap, data, len, &lim, &why) != PROVEN_OK) {
        rp_diag_error(RP_DIAG_BAD_PACKAGE, "'%s' is not a valid package: %s", input, why ? why : "unreadable");
        rp_mem_free(heap, data);
        return RP_EXIT_IO;
    }
    if (rp_msi_open(&msi, heap, &cfb, &lim, &why) != PROVEN_OK) {
        rp_diag_error(RP_DIAG_BAD_PACKAGE, "'%s' is not a valid MSI database: %s", input, why ? why : "unreadable");
        rp_cfb_close(&cfb);
        rp_mem_free(heap, data);
        return RP_EXIT_IO;
    }
    // The architecture from the summary's Template ("x64;1033", "Intel;1033", "Arm64;1033").
    int arch = 1;
    static char subject[256];
    subject[0] = '\0';
    uint32_t sid;
    if (rp_cfb_find(&cfb, 0, summary_name, sizeof summary_name / sizeof summary_name[0], &sid) == PROVEN_OK && cfb.entries[sid].size <= (1u << 20)) {
        size_t n = (size_t)cfb.entries[sid].size;
        uint8_t *buf = rp_mem_alloc(heap, n + 1, 1);
        rp_suminfo_t si;
        if (buf && rp_cfb_read(&cfb, sid, buf, n) == PROVEN_OK && rp_suminfo_parse(buf, n, &si) == PROVEN_OK) {
            for (size_t i = 0; i < si.count; ++i) {
                if (si.props[i].pid == 3 && si.props[i].type == RP_VT_LPSTR && si.props[i].str_len < sizeof subject) {
                    memcpy(subject, si.props[i].str, si.props[i].str_len);
                    subject[si.props[i].str_len] = '\0';
                }
                if (si.props[i].pid != 7 || si.props[i].type != RP_VT_LPSTR) continue;
                if (si.props[i].str_len >= 3 && (memcmp(si.props[i].str, "x64", 3) == 0 || memcmp(si.props[i].str, "Intel64", 7) == 0 ||
                                                 memcmp(si.props[i].str, "AMD64", 5) == 0)) {
                    arch = 0;
                } else if (si.props[i].str_len >= 5 && (memcmp(si.props[i].str, "Arm64", 5) == 0 || memcmp(si.props[i].str, "ARM64", 5) == 0)) {
                    arch = 2;
                }
            }
        }
        rp_mem_free(heap, buf);
    }
    if (rp_msi_view(&msi, NULL, 0, &view) != PROVEN_OK) {
        rp_diag_error(RP_DIAG_BAD_PACKAGE, "'%s': a table cannot be read", input);
        rp_msi_close(&msi);
        rp_cfb_close(&cfb);
        rp_mem_free(heap, data);
        return RP_EXIT_IO;
    }
    cx_t c = { .alloc = heap, .db = &view.db, .cfb = &cfb, .arch = arch, .notes = rp_buf_new(heap, 1u << 20), .subject = subject,
               .publisher = publisher };
    // The dist folder as the source names it: relative to the source's folder.
    const char *src_slash = strrchr(source, '/');
    size_t src_dir = src_slash ? (size_t)(src_slash - source) + 1 : 0;
    const char *dist_rel = src_dir && strncmp(dist, source, src_dir) == 0 ? dist + src_dir : dist;
    const char *base = src_slash ? src_slash + 1 : source;
    rp_buf_t b = rp_buf_new(heap, 1u << 26);
    const char *in_base = strrchr(input, '/');
    rc = write_source(&c, &b, in_base ? in_base + 1 : input, dist, dist_rel, base);
    if (rc == RP_EXIT_OK && (b.err != PROVEN_OK || rp_pal_write_file_new(heap, source, b.data, b.len) != PROVEN_OK)) {
        rp_diag_error(RP_DIAG_OUTPUT, "cannot write '%s'", source);
        rc = RP_EXIT_IO;
    }
    if (rc == RP_EXIT_OK) {
        char line[512];
        snprintf(line, sizeof line, "wrote %s and %s/ (build it with: rubrapack build %s -o <package>.msi)\n", source, dist, source);
        (void)rp_pal_puts(RP_OUT_STDOUT, line);
    }
    rp_buf_free(&b);
    rp_buf_free(&c.notes);
    for (size_t i = 0; i < c.nowned; ++i) rp_mem_free(heap, c.owned[i]);
    rp_mem_free(heap, c.owned);
    rp_mem_free(heap, c.ids);
    rp_mem_free(heap, c.ren_from);
    rp_mem_free(heap, c.ren_to);
    rp_mem_free(heap, c.ren_kind);
    rp_msi_view_free(&msi, &view);
    rp_msi_close(&msi);
    rp_cfb_close(&cfb);
    rp_mem_free(heap, data);
    return rc;
}
