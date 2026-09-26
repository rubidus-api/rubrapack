// src/model/ir.c - `.rpk` AST -> checked package model (include/rubrapack/ir.h, RFC-0002).

#include "rubrapack/buf.h"
#include "rubrapack/ir.h"
#include "rubrapack/mem.h"
#include "rubrapack/pal.h"
#include "rubrapack/text.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    proven_allocator_t     alloc;
    const rp_tdoc_t       *doc;
    const rp_ir_options_t *opt;
    rp_srcdiags_t         *d;
    rp_ir_t               *ir;
    const rp_ttable_t     *define;
    bool                   nomem;
} ctx_t;

#define ERR(c, pos, code, ...) rp_srcdiag_add((c)->d, (pos), (code), false, __VA_ARGS__)

static char *dup_n(ctx_t *c, const char *s, size_t n) {
    char *r = rp_mem_alloc(c->alloc, n + 1, 1);
    if (r == NULL) {
        c->nomem = true;
        return NULL;
    }
    memcpy(r, s, n);
    r[n] = '\0';
    return r;
}

static char *dup(ctx_t *c, const char *s) { return s ? dup_n(c, s, strlen(s)) : NULL; }

// ---- names and suggestions -------------------------------------------------------------------

static size_t edit_distance(const char *a, const char *b) {
    size_t na = strlen(a), nb = strlen(b);
    if (na > 40 || nb > 40) return 99;
    size_t prev[41], cur[41];
    for (size_t j = 0; j <= nb; ++j) prev[j] = j;
    for (size_t i = 1; i <= na; ++i) {
        cur[0] = i;
        for (size_t j = 1; j <= nb; ++j) {
            size_t sub = prev[j - 1] + (a[i - 1] != b[j - 1]);
            size_t del = prev[j] + 1, ins = cur[j - 1] + 1;
            cur[j] = sub < del ? (sub < ins ? sub : ins) : (del < ins ? del : ins);
        }
        memcpy(prev, cur, (nb + 1) * sizeof *prev);
    }
    return prev[nb];
}

static const char *suggest(const char *word, const char *const *choices) {
    const char *best = NULL;
    size_t best_d = 3;
    for (size_t k = 0; choices[k]; ++k) {
        size_t d = edit_distance(word, choices[k]);
        if (d < best_d) {
            best_d = d;
            best = choices[k];
        }
    }
    return best;
}

static bool valid_id(const char *s, size_t max) {
    size_t n = strlen(s);
    if (n == 0 || n > max) return false;
    if (!((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z') || s[0] == '_')) return false;
    for (size_t k = 1; k < n; ++k) {
        char ch = s[k];
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_')) return false;
    }
    return true;
}

static const char *const standard_properties[] = {
    "TARGETDIR", "SourceDir", "ProgramFiles64Folder", "ProgramFilesFolder", "CommonFiles64Folder", "CommonFilesFolder",
    "AppDataFolder", "LocalAppDataFolder", "CommonAppDataFolder", "StartMenuFolder", "ProgramMenuFolder",
    "DesktopFolder", "WindowsFolder", "SystemFolder", "System64Folder", "FontsFolder", "TempFolder", NULL,
};

// IDs that would collide with generated keys (DECISIONS 2026-09-26 P2 identity rules).
static bool reserved_id(const char *s) {
    for (size_t k = 0; standard_properties[k]; ++k) {
        if (strcmp(s, standard_properties[k]) == 0) return true;
    }
    if (strlen(s) == 22 && (s[0] == 'C' || s[0] == 'D' || s[0] == 'F') && s[1] == '_') {
        for (size_t k = 2; k < 22; ++k) {
            char ch = s[k];
            if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) return false;
        }
        return true;
    }
    return false;
}

static bool check_id(ctx_t *c, const rp_ttable_t *t, size_t max) {
    if (!valid_id(t->id, max)) {
        ERR(c, t->pos, "RP1301", "'%s' is not a valid ID (letters, digits and '_', not starting with a digit, at most %zu)",
            t->id, max);
        return false;
    }
    if (reserved_id(t->id)) {
        ERR(c, t->pos, "RP1302", "'%s' is reserved and cannot be used as an ID", t->id);
        return false;
    }
    return true;
}

// ---- keys ------------------------------------------------------------------------------------

static void check_keys(ctx_t *c, const rp_ttable_t *t, const char *const *allowed) {
    for (size_t k = 0; k < t->count; ++k) {
        bool known = false;
        for (size_t j = 0; allowed[j]; ++j) known |= strcmp(t->keys[k].key, allowed[j]) == 0;
        if (!known) {
            const char *hint = suggest(t->keys[k].key, allowed);
            ERR(c, t->keys[k].pos, "RP1201", "unknown key '%s' in [%s%s%s]%s%s%s", t->keys[k].key, t->kind, t->id ? "." : "",
                t->id ? t->id : "", hint ? " (did you mean '" : "", hint ? hint : "", hint ? "'?)" : "");
        }
    }
}

static const rp_tkey_t *find_key(const rp_ttable_t *t, const char *key) {
    for (size_t k = 0; k < t->count; ++k) {
        if (strcmp(t->keys[k].key, key) == 0) return &t->keys[k];
    }
    return NULL;
}

// $(NAME) substitution, once (RFC-0002 5): -D first, then [define]; values are not re-read.
static char *subst(ctx_t *c, const rp_tval_t *v) {
    rp_buf_t b = rp_buf_new(c->alloc, (size_t)1 << 24);
    const char *s = v->str;
    bool ok = true;
    for (size_t i = 0; i < v->len;) {
        if (s[i] == '$' && i + 1 < v->len && s[i + 1] == '$') {
            rp_buf_byte(&b, '$');
            i += 2;
            continue;
        }
        if (s[i] == '$' && i + 1 < v->len && s[i + 1] == '(') {
            const char *close = memchr(s + i + 2, ')', v->len - i - 2);
            if (close == NULL) {
                ERR(c, v->pos, "RP1403", "'$(' is not closed (write '$$(' for the text '$(')");
                ok = false;
                break;
            }
            size_t nl = (size_t)(close - (s + i + 2));
            char name[128];
            if (nl == 0 || nl >= sizeof name) {
                ERR(c, v->pos, "RP1403", "bad variable name in '$(...)'");
                ok = false;
                break;
            }
            memcpy(name, s + i + 2, nl);
            name[nl] = '\0';
            const char *val = NULL;
            for (size_t k = 0; k < c->opt->define_count && !val; ++k) {
                if (strcmp(c->opt->defines[k].name, name) == 0) val = c->opt->defines[k].value;
            }
            if (!val && c->define) {
                const rp_tkey_t *k = find_key(c->define, name);
                if (k && k->val.kind == RP_TV_STRING) val = k->val.str;
            }
            if (!val) {
                ERR(c, v->pos, "RP1403", "variable '%s' is not defined ([define] or -D %s=...)", name, name);
                ok = false;
                break;
            }
            rp_buf_puts(&b, val);
            i += nl + 3;
            continue;
        }
        rp_buf_byte(&b, (uint8_t)s[i]);
        ++i;
    }
    uint8_t *out = NULL;
    size_t n = 0;
    if (rp_buf_take(&b, &out, &n) != PROVEN_OK) {
        c->nomem = true;
        return NULL;
    }
    if (!ok) {
        rp_mem_free(c->alloc, out);
        return NULL;
    }
    char *r = dup_n(c, (const char *)out, n);
    rp_mem_free(c->alloc, out);
    return r;
}

// String value of a key (after substitution); *present tells whether the key was there.
static char *get_str(ctx_t *c, const rp_ttable_t *t, const char *key, bool required, bool *present) {
    const rp_tkey_t *k = find_key(t, key);
    if (present) *present = k != NULL;
    if (k == NULL) {
        if (required) {
            ERR(c, t->pos, "RP1202", "[%s%s%s] needs '%s'", t->kind, t->id ? "." : "", t->id ? t->id : "", key);
        }
        return NULL;
    }
    if (k->val.kind != RP_TV_STRING) {
        ERR(c, k->pos, "RP1306", "'%s' must be a string", key);
        return NULL;
    }
    char *s = subst(c, &k->val);
    if (s && s[0] == '\0' && required) {
        ERR(c, k->pos, "RP1305", "'%s' must not be empty", key);
        rp_mem_free(c->alloc, s);
        return NULL;
    }
    return s;
}

static bool get_bool(ctx_t *c, const rp_ttable_t *t, const char *key, bool dflt) {
    const rp_tkey_t *k = find_key(t, key);
    if (k == NULL) return dflt;
    if (k->val.kind != RP_TV_BOOL) {
        ERR(c, k->pos, "RP1306", "'%s' must be true or false", key);
        return dflt;
    }
    return k->val.b;
}

static int64_t get_int(ctx_t *c, const rp_ttable_t *t, const char *key, int64_t dflt, int64_t lo, int64_t hi) {
    const rp_tkey_t *k = find_key(t, key);
    if (k == NULL) return dflt;
    if (k->val.kind != RP_TV_INT) {
        ERR(c, k->pos, "RP1306", "'%s' must be an integer", key);
        return dflt;
    }
    if (k->val.i < lo || k->val.i > hi) {
        ERR(c, k->pos, "RP1308", "'%s' must be between %lld and %lld", key, (long long)lo, (long long)hi);
        return dflt;
    }
    return k->val.i;
}

static rp_pos_t key_pos(const rp_ttable_t *t, const char *key) {
    const rp_tkey_t *k = find_key(t, key);
    return k ? k->pos : t->pos;
}

// ---- value checks -------------------------------------------------------------------------

static bool is_hex(char ch) { return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F'); }

// {8-4-4-4-12}, normalised to upper case in place.
static bool guid_ok(char *s) {
    static const int groups[5] = { 8, 4, 4, 4, 12 };
    if (strlen(s) != 38 || s[0] != '{' || s[37] != '}') return false;
    size_t p = 1;
    for (int g = 0; g < 5; ++g) {
        for (int k = 0; k < groups[g]; ++k, ++p) {
            if (!is_hex(s[p])) return false;
            if (s[p] >= 'a' && s[p] <= 'f') s[p] = (char)(s[p] - 32);
        }
        if (g < 4 && s[p++] != '-') return false;
    }
    return true;
}

static bool ascii_only(const char *s) {
    for (; *s; ++s) {
        if ((unsigned char)*s >= 0x80) return false;
    }
    return true;
}

static bool has_control(const char *s) {
    for (; *s; ++s) {
        if ((unsigned char)*s < 0x20 || (unsigned char)*s == 0x7F) return true;
    }
    return false;
}

// RFC-0002 8.2 / RFC-0001 14.3: one target path component.
static bool target_name_ok(ctx_t *c, const char *name, rp_pos_t pos) {
    size_t n = strlen(name);
    const char *why = NULL;
    if (n == 0) why = "is empty";
    else if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) why = "is '.' or '..'";
    else if (has_control(name)) why = "contains a control character";
    else if (strpbrk(name, "<>:\"/\\|?*")) why = "contains one of < > : \" / \\ | ? *";
    else if (name[n - 1] == ' ' || name[n - 1] == '.') why = "ends with a space or a dot";
    else {
        uint16_t units[300];
        rp_text_result_t r = rp_utf8_to_utf16((const uint8_t *)name, n, units, 300);
        if (r.err != PROVEN_OK || r.units > 255) why = "is longer than 255 UTF-16 units";
    }
    if (why == NULL) {
        static const char *const devices[] = { "CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5",
                                               "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4",
                                               "LPT5", "LPT6", "LPT7", "LPT8", "LPT9", NULL };
        size_t stem = strcspn(name, ".");
        for (size_t k = 0; devices[k]; ++k) {
            size_t dl = strlen(devices[k]);
            if (stem != dl) continue;
            bool same = true;
            for (size_t j = 0; j < dl; ++j) {
                char ch = name[j];
                if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
                same &= ch == devices[k][j];
            }
            if (same) why = "is a reserved device name";
        }
    }
    if (why) {
        ERR(c, pos, "RP1510", "target name '%s' %s", name, why);
        return false;
    }
    return true;
}

// Case folding for collision checks: ASCII and Latin-1 letters (Windows compares names
// case-insensitively; Hangul and CJK have no case).
static void fold(const char *s, char *out, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; s[i] && o + 3 < cap;) {
        unsigned char ch = (unsigned char)s[i];
        if (ch >= 'A' && ch <= 'Z') {
            out[o++] = (char)(ch + 32);
            ++i;
        } else if (ch == 0xC3 && (unsigned char)s[i + 1] >= 0x80 && (unsigned char)s[i + 1] <= 0x9E &&
                   (unsigned char)s[i + 1] != 0x97) {
            out[o++] = (char)0xC3;
            out[o++] = (char)((unsigned char)s[i + 1] + 0x20);
            i += 2;
        } else {
            out[o++] = (char)ch;
            ++i;
        }
    }
    out[o] = '\0';
}

// ---- known folders ---------------------------------------------------------------------------

static const char *const known_folders[] = {
    "ProgramFiles", "ProgramFiles32", "CommonFiles", "AppData", "LocalAppData", "CommonAppData", "StartMenu",
    "Programs", "Desktop", "Windows", "System", "Fonts", "Temp", NULL,
};

static bool known_folder(const char *s) {
    for (size_t k = 0; known_folders[k]; ++k) {
        if (strcmp(s, known_folders[k]) == 0) return true;
    }
    return false;
}

// ---- tables ----------------------------------------------------------------------------------

static const char *const top_kinds[] = { "package", "define", NULL };
static const char *const item_kinds[] = { "feature", "dir", "file", NULL };
static const char *const later_kinds[] = { "files", "folder", "registry", "shortcut", "env", "ini", "service",
                                           "assoc", "protocol", "font", "permission", "require", "search",
                                           "remove", "copy", "action", "arp", "ui", "ui-text", "msix",
                                           "msix-app", "msix-extension", NULL };
static const char *const all_kinds[] = { "package", "define", "feature", "dir", "file", "files", "folder",
                                         "registry", "shortcut", "env", "ini", "service", "assoc", "protocol",
                                         "font", "permission", "require", "search", "remove", "copy", "action",
                                         "arp", "ui", "ui-text", "msix", "msix-app", "msix-extension", NULL };

static bool in_list(const char *s, const char *const *list) {
    for (size_t k = 0; list[k]; ++k) {
        if (strcmp(s, list[k]) == 0) return true;
    }
    return false;
}

static const char *phase_of(const char *kind) {
    if (strcmp(kind, "files") == 0 || strcmp(kind, "folder") == 0) return "later in P2";
    if (strcmp(kind, "action") == 0) return "P3";
    if (strncmp(kind, "ui", 2) == 0) return "P4";
    if (strncmp(kind, "msix", 4) == 0) return "P8";
    return "P3";
}

static void parse_package(ctx_t *c, const rp_ttable_t *t) {
    static const char *const keys[] = { "name", "summary-name", "manufacturer", "version", "arch", "upgrade-code",
                                        "product-code", "scope", "language", "ui", "license", "icon", "reboot",
                                        "downgrade-message", "compress", "cab", NULL };
    rp_ir_t *ir = c->ir;
    check_keys(c, t, keys);
    ir->name = get_str(c, t, "name", true, NULL);
    ir->manufacturer = get_str(c, t, "manufacturer", true, NULL);
    ir->summary_name = get_str(c, t, "summary-name", false, NULL);
    if (ir->summary_name && !ascii_only(ir->summary_name)) {
        ERR(c, key_pos(t, "summary-name"), "RP1308", "summary-name must be ASCII (summary information holds ASCII only)");
    }
    if (ir->name && has_control(ir->name)) ERR(c, key_pos(t, "name"), "RP1308", "name contains a control character");

    ir->version = get_str(c, t, "version", true, NULL);
    if (ir->version) {
        static const unsigned max[4] = { 255, 255, 65535, 65535 };
        const char *p = ir->version;
        bool ok = true;
        size_t n = 0;
        while (ok) {
            unsigned long v = 0;
            size_t digits = 0;
            while (*p >= '0' && *p <= '9' && digits < 6) v = v * 10 + (unsigned long)(*p++ - '0'), ++digits;
            if (digits == 0 || n >= 4 || v > max[n]) ok = false;
            else ir->version_parts[n++] = (uint16_t)v;
            if (*p == '.') ++p;
            else break;
        }
        if (!ok || *p != '\0' || n < 3) {
            ERR(c, key_pos(t, "version"), "RP1308",
                "version '%s' must be a.b.c or a.b.c.d with a, b <= 255 and c, d <= 65535", ir->version);
        }
        ir->version_count = n;
    }

    char *arch = c->opt->arch ? dup(c, c->opt->arch) : get_str(c, t, "arch", true, NULL);
    if (arch) {
        if (strcmp(arch, "x64") == 0) ir->arch = RP_ARCH_X64;
        else if (strcmp(arch, "arm64") == 0) ir->arch = RP_ARCH_ARM64;
        else if (strcmp(arch, "x86") == 0) ir->arch = RP_ARCH_X86;
        else ERR(c, key_pos(t, "arch"), "RP1308", "arch must be \"x64\", \"arm64\" or \"x86\" (got '%s')", arch);
        rp_mem_free(c->alloc, arch);
    }

    ir->upgrade_code = get_str(c, t, "upgrade-code", true, NULL);
    if (ir->upgrade_code && !guid_ok(ir->upgrade_code)) {
        ERR(c, key_pos(t, "upgrade-code"), "RP1308", "upgrade-code must be a GUID like {12345678-1234-1234-1234-123456789ABC}");
    }
    ir->product_code = get_str(c, t, "product-code", false, NULL);
    if (ir->product_code && !guid_ok(ir->product_code)) {
        ERR(c, key_pos(t, "product-code"), "RP1308", "product-code must be a GUID");
    }

    char *scope = get_str(c, t, "scope", false, NULL);
    if (scope && strcmp(scope, "machine") != 0) {
        if (strcmp(scope, "user") == 0 || strcmp(scope, "dual") == 0) {
            ERR(c, key_pos(t, "scope"), "RP1901", "scope \"%s\" is not supported yet (planned for P3)", scope);
        } else {
            ERR(c, key_pos(t, "scope"), "RP1308", "scope must be \"machine\", \"user\" or \"dual\"");
        }
    }
    rp_mem_free(c->alloc, scope);

    ir->language = 1033;
    char *lang = get_str(c, t, "language", false, NULL);
    if (lang) {
        if (strcmp(lang, "ko-KR") == 0) ir->language = 1042;
        else if (strcmp(lang, "en-US") != 0) ERR(c, key_pos(t, "language"), "RP1308", "language must be \"ko-KR\" or \"en-US\"");
        rp_mem_free(c->alloc, lang);
    }

    char *ui = get_str(c, t, "ui", false, NULL);
    if (ui && strcmp(ui, "none") != 0) {
        ERR(c, key_pos(t, "ui"), "RP1901", "ui \"%s\" is not supported yet (planned for P4); use \"none\"", ui);
    }
    rp_mem_free(c->alloc, ui);
    if (find_key(t, "license")) ERR(c, key_pos(t, "license"), "RP1901", "license is not supported yet (planned for P4)");
    if (find_key(t, "icon")) ERR(c, key_pos(t, "icon"), "RP1901", "icon is not supported yet (planned for P3)");

    ir->reboot_suppress = true;
    char *reboot = get_str(c, t, "reboot", false, NULL);
    if (reboot) {
        if (strcmp(reboot, "allow") == 0) ir->reboot_suppress = false;
        else if (strcmp(reboot, "suppress") != 0) ERR(c, key_pos(t, "reboot"), "RP1308", "reboot must be \"suppress\" or \"allow\"");
        rp_mem_free(c->alloc, reboot);
    }
    ir->downgrade_message = get_str(c, t, "downgrade-message", false, NULL);

    char *comp = c->opt->compress ? dup(c, c->opt->compress) : get_str(c, t, "compress", false, NULL);
    ir->compress = 6;
    if (comp) {
        if (strcmp(comp, "none") == 0) ir->compress = -1;
        else if (strcmp(comp, "mszip") == 0) ir->compress = 6;
        else if (strncmp(comp, "mszip:", 6) == 0 && comp[6] >= '0' && comp[6] <= '9' && comp[7] == '\0') ir->compress = comp[6] - '0';
        else ERR(c, key_pos(t, "compress"), "RP1308", "compress must be \"none\", \"mszip\" or \"mszip:0\" ... \"mszip:9\"");
        rp_mem_free(c->alloc, comp);
    }
    if (ir->compress >= 0) {
        ERR(c, key_pos(t, "compress"), "RP1901",
            "MSZIP compression is not implemented yet (later in P2); set compress = \"none\" for now");
    }
    char *cab = get_str(c, t, "cab", false, NULL);
    if (cab && strcmp(cab, "embed") != 0) {
        ERR(c, key_pos(t, "cab"), strcmp(cab, "external") == 0 ? "RP1901" : "RP1308",
            "cab must be \"embed\" for now (external cabinets are planned for P3)");
    }
    rp_mem_free(c->alloc, cab);
}

static void parse_feature(ctx_t *c, const rp_ttable_t *t, rp_ir_feature_t *f) {
    static const char *const keys[] = { "title", "description", "level", "hidden", "parent", "when", NULL };
    check_keys(c, t, keys);
    check_id(c, t, 38);
    f->id = dup(c, t->id);
    f->pos = t->pos;
    f->title = get_str(c, t, "title", true, NULL);
    f->description = get_str(c, t, "description", false, NULL);
    f->level = (int32_t)get_int(c, t, "level", 1, 1, 32767);
    f->hidden = get_bool(c, t, "hidden", false);
    f->parent = get_str(c, t, "parent", false, NULL);
    if (find_key(t, "when")) ERR(c, key_pos(t, "when"), "RP1901", "feature conditions are not supported yet (planned for P3)");
}

static void parse_dir(ctx_t *c, const rp_ttable_t *t, rp_ir_dir_t *d) {
    static const char *const keys[] = { "path", "feature", NULL };
    check_keys(c, t, keys);
    check_id(c, t, 72);
    d->id = dup(c, t->id);
    d->pos = t->pos;
    d->feature = get_str(c, t, "feature", false, NULL);
    char *path = get_str(c, t, "path", true, NULL);
    if (path == NULL) return;
    // base/part/part...
    size_t count = 1;
    for (const char *p = path; *p; ++p) count += *p == '/';
    if (count < 2 || strchr(path, '\\')) {
        ERR(c, key_pos(t, "path"), "RP1308", "path must be \"Base/relative/path\" with '/' (Base is a known folder or a dir ID)");
        rp_mem_free(c->alloc, path);
        return;
    }
    d->parts = rp_mem_alloc(c->alloc, count - 1, sizeof *d->parts);
    if (d->parts == NULL) {
        c->nomem = true;
        rp_mem_free(c->alloc, path);
        return;
    }
    char *save = path;
    char *slash = strchr(save, '/');
    char *base = dup_n(c, save, (size_t)(slash - save));
    if (base && known_folder(base)) d->base = base;
    else d->parent = base;
    save = slash + 1;
    for (size_t k = 0; k < count - 1; ++k) {
        char *next = strchr(save, '/');
        size_t n = next ? (size_t)(next - save) : strlen(save);
        d->parts[k] = dup_n(c, save, n);
        if (d->parts[k]) target_name_ok(c, d->parts[k], key_pos(t, "path"));
        d->part_count++;
        save = next ? next + 1 : save + n;
    }
    rp_mem_free(c->alloc, path);
}

static void parse_file(ctx_t *c, const rp_ttable_t *t, rp_ir_file_t *f) {
    static const char *const keys[] = { "dir", "source", "name", "any-arch", "keep", "vital", "feature",
                                        "component-guid", NULL };
    check_keys(c, t, keys);
    check_id(c, t, 72);
    f->id = dup(c, t->id);
    f->pos = t->pos;
    f->dir = get_str(c, t, "dir", true, NULL);
    f->source = get_str(c, t, "source", true, NULL);
    f->name = get_str(c, t, "name", false, NULL);
    f->any_arch = get_bool(c, t, "any-arch", false);
    f->keep = get_bool(c, t, "keep", false);
    f->vital = get_bool(c, t, "vital", true);
    f->feature = get_str(c, t, "feature", false, NULL);
    f->component_guid = get_str(c, t, "component-guid", false, NULL);
    if (f->component_guid && !guid_ok(f->component_guid)) {
        ERR(c, key_pos(t, "component-guid"), "RP1308", "component-guid must be a GUID");
    }
    if (f->keep) ERR(c, key_pos(t, "keep"), "RP1901", "keep for files is not supported yet (planned for P3)");
    if (f->source == NULL) return;
    rp_pos_t sp = key_pos(t, "source");
    const char *s = f->source;
    if (strchr(s, '\\')) {
        ERR(c, sp, "RP1502", "source paths use '/', not '\\\\' (got '%s')", s);
        return;
    }
    if (s[0] == '/' || (s[0] && s[1] == ':')) {
        ERR(c, sp, "RP1501", "source path '%s' must be relative to the .rpk file", s);
        return;
    }
    const char *dir = c->opt->source_dir ? c->opt->source_dir : ".";
    size_t n = strlen(dir) + strlen(s) + 2;
    f->source_path = rp_mem_alloc(c->alloc, n, 1);
    if (f->source_path == NULL) {
        c->nomem = true;
        return;
    }
    snprintf(f->source_path, n, "%s/%s", dir, s);
    rp_fskind_t kind = rp_pal_stat(c->alloc, f->source_path, &f->size);
    if (kind == RP_FS_NONE) ERR(c, sp, "RP1507", "source file '%s' not found", s);
    else if (kind == RP_FS_LINK) ERR(c, sp, "RP1504", "source '%s' is a symbolic link; links are not followed", s);
    else if (kind == RP_FS_DIR) ERR(c, sp, "RP1508", "source '%s' is a directory; use [files.ID] for many files", s);
    else if (kind != RP_FS_FILE) ERR(c, sp, "RP1508", "source '%s' is not a regular file", s);
    if (f->name == NULL) {
        const char *b = strrchr(s, '/');
        f->name = dup(c, b ? b + 1 : s);
    }
    if (f->name) target_name_ok(c, f->name, find_key(t, "name") ? key_pos(t, "name") : sp);
}

// ---- cross checks --------------------------------------------------------------------------

static const rp_ir_dir_t *find_dir(const rp_ir_t *ir, const char *id) {
    for (size_t k = 0; k < ir->dir_count; ++k) {
        if (ir->dirs[k].id && strcmp(ir->dirs[k].id, id) == 0) return &ir->dirs[k];
    }
    return NULL;
}

static const rp_ir_feature_t *find_feature(const rp_ir_t *ir, const char *id) {
    for (size_t k = 0; k < ir->feature_count; ++k) {
        if (ir->features[k].id && strcmp(ir->features[k].id, id) == 0) return &ir->features[k];
    }
    return NULL;
}

// Full target path of a dir (known folder / parts...), or NULL when the chain is broken or loops.
static char *dir_full_path(ctx_t *c, const rp_ir_dir_t *d, size_t depth) {
    if (depth > c->ir->dir_count || d->parts == NULL) return NULL;
    char *head;
    if (d->base) {
        head = dup(c, d->base);
    } else {
        const rp_ir_dir_t *p = d->parent ? find_dir(c->ir, d->parent) : NULL;
        head = p ? dir_full_path(c, p, depth + 1) : NULL;
    }
    if (head == NULL) return NULL;
    rp_buf_t b = rp_buf_new(c->alloc, 1u << 16);
    rp_buf_puts(&b, head);
    for (size_t k = 0; k < d->part_count; ++k) {
        rp_buf_byte(&b, '/');
        rp_buf_puts(&b, d->parts[k] ? d->parts[k] : "");
    }
    rp_buf_byte(&b, 0);
    rp_mem_free(c->alloc, head);
    uint8_t *out;
    size_t n;
    if (rp_buf_take(&b, &out, &n) != PROVEN_OK) return NULL;
    return (char *)out;
}

static void cross_checks(ctx_t *c) {
    rp_ir_t *ir = c->ir;
    // IDs unique across dir, file and feature (RFC-0002 2).
    typedef struct { const char *id; rp_pos_t pos; } idpos_t;
    size_t n = ir->dir_count + ir->file_count + ir->feature_count;
    idpos_t *ids = rp_mem_alloc(c->alloc, n, sizeof *ids);
    if (ids == NULL) {
        c->nomem = true;
        return;
    }
    size_t m = 0;
    for (size_t k = 0; k < ir->dir_count; ++k) ids[m++] = (idpos_t){ ir->dirs[k].id, ir->dirs[k].pos };
    for (size_t k = 0; k < ir->file_count; ++k) ids[m++] = (idpos_t){ ir->files[k].id, ir->files[k].pos };
    for (size_t k = 0; k < ir->feature_count; ++k) {
        if (!ir->features[k].implicit) ids[m++] = (idpos_t){ ir->features[k].id, ir->features[k].pos };
    }
    for (size_t a = 0; a < m; ++a) {
        for (size_t b = a + 1; b < m; ++b) {
            if (ids[a].id && ids[b].id && strcmp(ids[a].id, ids[b].id) == 0) {
                ERR(c, ids[b].pos, "RP1301", "ID '%s' is already used (line %u); dir, file and feature IDs must differ",
                    ids[b].id, (unsigned)ids[a].pos.line);
            }
        }
    }
    rp_mem_free(c->alloc, ids);

    // Features: parents exist, no loops.
    for (size_t k = 0; k < ir->feature_count; ++k) {
        const rp_ir_feature_t *f = &ir->features[k];
        size_t steps = 0;
        for (const rp_ir_feature_t *p = f; p && p->parent; p = find_feature(ir, p->parent)) {
            if (!find_feature(ir, p->parent)) {
                ERR(c, p->pos, "RP1307", "feature parent '%s' is not defined", p->parent);
                break;
            }
            if (++steps > ir->feature_count) {
                ERR(c, f->pos, "RP1303", "feature '%s' is its own ancestor", f->id);
                break;
            }
        }
    }

    // Dirs: parents exist, no loops; collect full paths for collision checks.
    char **dir_paths = rp_mem_alloc(c->alloc, ir->dir_count, sizeof *dir_paths);
    if (dir_paths == NULL) {
        c->nomem = true;
        return;
    }
    for (size_t k = 0; k < ir->dir_count; ++k) {
        const rp_ir_dir_t *d = &ir->dirs[k];
        dir_paths[k] = NULL;
        if (d->parts == NULL) continue;
        if (d->parent && !find_dir(ir, d->parent)) {
            ERR(c, d->pos, "RP1307", "'%s' is neither a known folder nor a dir ID (known folders: ProgramFiles, "
                "ProgramFiles32, CommonFiles, AppData, LocalAppData, CommonAppData, StartMenu, Programs, Desktop, "
                "Windows, System, Fonts, Temp)", d->parent);
            continue;
        }
        dir_paths[k] = dir_full_path(c, d, 0);
        if (dir_paths[k] == NULL && d->parent) ERR(c, d->pos, "RP1303", "dir '%s' is inside itself (path loop)", d->id);
        if (d->feature && !find_feature(ir, d->feature)) {
            ERR(c, d->pos, "RP1307", "feature '%s' is not defined", d->feature);
        }
    }

    // Files: dir exists, feature resolves (G2), no PE yet, and no two paths differ only by case.
    bool declared = false;
    for (size_t k = 0; k < ir->feature_count; ++k) declared |= !ir->features[k].implicit;
    size_t np = ir->dir_count + ir->file_count;
    char **folded = rp_mem_alloc(c->alloc, np, sizeof *folded);
    rp_pos_t *where = rp_mem_alloc(c->alloc, np, sizeof *where);
    size_t nf = 0;
    for (size_t k = 0; k < ir->dir_count && folded; ++k) {
        if (dir_paths[k] == NULL) continue;
        folded[nf] = rp_mem_alloc(c->alloc, strlen(dir_paths[k]) * 2 + 4, 1);
        if (folded[nf]) fold(dir_paths[k], folded[nf], strlen(dir_paths[k]) * 2 + 4);
        where[nf++] = ir->dirs[k].pos;
    }
    for (size_t k = 0; k < ir->file_count; ++k) {
        rp_ir_file_t *f = &ir->files[k];
        const rp_ir_dir_t *d = f->dir ? find_dir(ir, f->dir) : NULL;
        if (f->dir && d == NULL) {
            ERR(c, f->pos, "RP1307", "dir '%s' is not defined", f->dir);
            continue;
        }
        if (f->feature && !find_feature(ir, f->feature)) {
            ERR(c, f->pos, "RP1307", "feature '%s' is not defined", f->feature);
        } else if (f->feature == NULL) {
            if (d && d->feature) f->feature = dup(c, d->feature);
            else if (!declared) f->feature = dup(c, "Main");
            else ERR(c, f->pos, "RP1202", "file '%s' needs a feature: set 'feature' here or on its dir", f->id);
        }
        if (d && f->name && folded) {
            size_t di = (size_t)(d - ir->dirs);
            if (dir_paths[di]) {
                size_t len = strlen(dir_paths[di]) + strlen(f->name) + 2;
                char *full = rp_mem_alloc(c->alloc, len, 1);
                if (full) {
                    snprintf(full, len, "%s/%s", dir_paths[di], f->name);
                    folded[nf] = rp_mem_alloc(c->alloc, len * 2 + 4, 1);
                    if (folded[nf]) fold(full, folded[nf], len * 2 + 4);
                    where[nf++] = f->pos;
                    rp_mem_free(c->alloc, full);
                }
            }
        }
        // PE files need the version reader (later in P2); refuse rather than package them wrongly.
        if (f->source_path && f->size >= 2) {
            uint8_t *data;
            size_t len;
            if (rp_pal_read_file(c->alloc, f->source_path, (size_t)f->size, &data, &len) == PROVEN_OK) {
                if (len >= 2 && data[0] == 'M' && data[1] == 'Z') {
                    ERR(c, f->pos, "RP1901", "'%s' is a program file (PE); version and machine checks come later in P2",
                        f->source);
                }
                rp_mem_free(c->alloc, data);
            }
        }
    }
    for (size_t a = 0; folded && a < nf; ++a) {
        for (size_t b = a + 1; b < nf; ++b) {
            if (folded[a] && folded[b] && strcmp(folded[a], folded[b]) == 0) {
                ERR(c, where[b], "RP1511", "installs to the same path as line %u (Windows ignores case in names)",
                    (unsigned)where[a].line);
            }
        }
    }
    for (size_t a = 0; folded && a < nf; ++a) rp_mem_free(c->alloc, folded[a]);
    rp_mem_free(c->alloc, folded);
    rp_mem_free(c->alloc, where);
    for (size_t k = 0; k < ir->dir_count; ++k) rp_mem_free(c->alloc, dir_paths[k]);
    rp_mem_free(c->alloc, dir_paths);
}

// ---- build -----------------------------------------------------------------------------------

proven_err_t rp_ir_build(proven_allocator_t alloc, const rp_tdoc_t *doc, const rp_ir_options_t *opt, rp_ir_t *ir,
                         rp_srcdiags_t *diags) {
    if (doc == NULL || opt == NULL || ir == NULL || diags == NULL) return PROVEN_ERR_INVALID_ARG;
    memset(ir, 0, sizeof *ir);
    ir->alloc = alloc;
    ctx_t c = { .alloc = alloc, .doc = doc, .opt = opt, .d = diags, .ir = ir };

    const rp_ttable_t *package = NULL;
    size_t nfeat = 0, ndir = 0, nfile = 0;
    for (size_t k = 0; k < doc->count; ++k) {
        const rp_ttable_t *t = &doc->tables[k];
        if (!in_list(t->kind, all_kinds)) {
            const char *hint = suggest(t->kind, all_kinds);
            ERR(&c, t->pos, "RP1201", "unknown table [%s%s%s]%s%s%s", t->kind, t->id ? "." : "", t->id ? t->id : "",
                hint ? " (did you mean [" : "", hint ? hint : "", hint ? "]?)" : "");
            continue;
        }
        if (in_list(t->kind, later_kinds)) {
            ERR(&c, t->pos, "RP1901", "[%s%s] is not supported yet (planned for %s)", t->kind, t->id ? ".*" : "",
                phase_of(t->kind));
            continue;
        }
        bool top = in_list(t->kind, top_kinds);
        if (top && t->id) {
            ERR(&c, t->pos, "RP1201", "[%s] takes no ID: write [%s]", t->kind, t->kind);
            continue;
        }
        if (!top && t->id == NULL) {
            ERR(&c, t->pos, "RP1201", "[%s] needs an ID: write [%s.ID]", t->kind, t->kind);
            continue;
        }
        if (strcmp(t->kind, "package") == 0) package = t;
        else if (strcmp(t->kind, "define") == 0) c.define = t;
        else if (strcmp(t->kind, "feature") == 0) ++nfeat;
        else if (strcmp(t->kind, "dir") == 0) ++ndir;
        else if (strcmp(t->kind, "file") == 0) ++nfile;
    }
    (void)item_kinds;
    if (c.define) {
        for (size_t k = 0; k < c.define->count; ++k) {
            const rp_tkey_t *key = &c.define->keys[k];
            if (!valid_id(key->key, 64)) ERR(&c, key->pos, "RP1401", "'%s' is not a valid variable name", key->key);
            if (key->val.kind != RP_TV_STRING) ERR(&c, key->pos, "RP1306", "[define] values must be strings");
        }
    }
    if (package == NULL) {
        rp_pos_t top = { 1, 1 };
        ERR(&c, top, "RP1202", "the file needs a [package] table");
    } else {
        parse_package(&c, package);
    }

    ir->features = rp_mem_alloc(alloc, nfeat + 1, sizeof *ir->features);
    ir->dirs = rp_mem_alloc(alloc, ndir, sizeof *ir->dirs);
    ir->files = rp_mem_alloc(alloc, nfile, sizeof *ir->files);
    if (ir->features == NULL || ir->dirs == NULL || ir->files == NULL) c.nomem = true;
    for (size_t k = 0; k < doc->count && !c.nomem; ++k) {
        const rp_ttable_t *t = &doc->tables[k];
        if (t->id == NULL) continue;
        if (strcmp(t->kind, "feature") == 0) {
            rp_ir_feature_t *f = &ir->features[ir->feature_count++];
            memset(f, 0, sizeof *f);
            parse_feature(&c, t, f);
        } else if (strcmp(t->kind, "dir") == 0) {
            rp_ir_dir_t *d = &ir->dirs[ir->dir_count++];
            memset(d, 0, sizeof *d);
            parse_dir(&c, t, d);
        } else if (strcmp(t->kind, "file") == 0) {
            rp_ir_file_t *f = &ir->files[ir->file_count++];
            memset(f, 0, sizeof *f);
            parse_file(&c, t, f);
        }
    }
    if (!c.nomem && ir->feature_count == 0) {       // G2: one hidden default feature
        rp_ir_feature_t *f = &ir->features[ir->feature_count++];
        memset(f, 0, sizeof *f);
        f->id = dup(&c, "Main");
        f->title = dup(&c, ir->name ? ir->name : "Main");
        f->level = 1;
        f->hidden = true;
        f->implicit = true;
    }
    if (!c.nomem) cross_checks(&c);
    if (c.nomem) {
        rp_ir_free(ir);
        return PROVEN_ERR_NOMEM;
    }
    if (diags->errors > 0) {
        rp_ir_free(ir);
        return PROVEN_ERR_INVALID_FORMAT;
    }
    return PROVEN_OK;
}

void rp_ir_free(rp_ir_t *ir) {
    if (ir == NULL) return;
    proven_allocator_t a = ir->alloc;
    char *strs[] = { ir->name, ir->summary_name, ir->manufacturer, ir->version, ir->upgrade_code, ir->product_code,
                     ir->downgrade_message };
    for (size_t k = 0; k < sizeof strs / sizeof strs[0]; ++k) rp_mem_free(a, strs[k]);
    for (size_t k = 0; k < ir->feature_count; ++k) {
        rp_ir_feature_t *f = &ir->features[k];
        rp_mem_free(a, f->id);
        rp_mem_free(a, f->title);
        rp_mem_free(a, f->description);
        rp_mem_free(a, f->parent);
    }
    for (size_t k = 0; k < ir->dir_count; ++k) {
        rp_ir_dir_t *d = &ir->dirs[k];
        rp_mem_free(a, d->id);
        rp_mem_free(a, d->base);
        rp_mem_free(a, d->parent);
        rp_mem_free(a, d->feature);
        for (size_t j = 0; j < d->part_count; ++j) rp_mem_free(a, d->parts[j]);
        rp_mem_free(a, d->parts);
    }
    for (size_t k = 0; k < ir->file_count; ++k) {
        rp_ir_file_t *f = &ir->files[k];
        char *fs[] = { f->id, f->dir, f->source, f->source_path, f->name, f->feature, f->component_guid };
        for (size_t j = 0; j < sizeof fs / sizeof fs[0]; ++j) rp_mem_free(a, fs[j]);
    }
    rp_mem_free(a, ir->features);
    rp_mem_free(a, ir->dirs);
    rp_mem_free(a, ir->files);
    memset(ir, 0, sizeof *ir);
    ir->alloc = a;
}

// ---- dump ------------------------------------------------------------------------------------

static void kv(rp_buf_t *b, const char *k, const char *v) {
    rp_buf_byte(b, ' ');
    rp_buf_puts(b, k);
    rp_buf_byte(b, '=');
    rp_buf_puts(b, v ? v : "-");
}

static int cmp_str(const char *a, const char *b) { return strcmp(a ? a : "", b ? b : ""); }

proven_err_t rp_ir_dump(const rp_ir_t *ir, proven_allocator_t alloc, uint8_t **out, size_t *len) {
    static const char *const archs[] = { "x64", "arm64", "x86" };
    char num[32];
    rp_buf_t b = rp_buf_new(alloc, (size_t)1 << 24);
    rp_buf_puts(&b, "package");
    kv(&b, "name", ir->name);
    kv(&b, "summary-name", ir->summary_name);
    kv(&b, "manufacturer", ir->manufacturer);
    kv(&b, "version", ir->version);
    kv(&b, "arch", archs[ir->arch]);
    kv(&b, "upgrade-code", ir->upgrade_code);
    kv(&b, "product-code", ir->product_code);
    snprintf(num, sizeof num, "%u", ir->language);
    kv(&b, "language", num);
    kv(&b, "reboot", ir->reboot_suppress ? "suppress" : "allow");
    kv(&b, "downgrade-message", ir->downgrade_message);
    snprintf(num, sizeof num, "%d", ir->compress);
    kv(&b, "compress", ir->compress < 0 ? "none" : num);
    rp_buf_byte(&b, '\n');

    // Each kind in ID order, so the dump does not depend on the order of tables in the source.
    size_t *idx = rp_mem_alloc(alloc, ir->feature_count + ir->dir_count + ir->file_count + 1, sizeof *idx);
    if (idx == NULL) {
        rp_buf_free(&b);
        return PROVEN_ERR_NOMEM;
    }
    for (int kind = 0; kind < 3; ++kind) {
        size_t n = kind == 0 ? ir->feature_count : kind == 1 ? ir->dir_count : ir->file_count;
        for (size_t k = 0; k < n; ++k) idx[k] = k;
        for (size_t i = 1; i < n; ++i) {
            for (size_t j = i; j > 0; --j) {
                const char *a = kind == 0 ? ir->features[idx[j - 1]].id : kind == 1 ? ir->dirs[idx[j - 1]].id : ir->files[idx[j - 1]].id;
                const char *bb = kind == 0 ? ir->features[idx[j]].id : kind == 1 ? ir->dirs[idx[j]].id : ir->files[idx[j]].id;
                if (cmp_str(a, bb) <= 0) break;
                size_t t = idx[j];
                idx[j] = idx[j - 1];
                idx[j - 1] = t;
            }
        }
        for (size_t k = 0; k < n; ++k) {
            if (kind == 0) {
                const rp_ir_feature_t *f = &ir->features[idx[k]];
                rp_buf_puts(&b, "feature ");
                rp_buf_puts(&b, f->id);
                kv(&b, "title", f->title);
                kv(&b, "description", f->description);
                snprintf(num, sizeof num, "%d", f->level);
                kv(&b, "level", num);
                kv(&b, "hidden", f->hidden ? "1" : "0");
                kv(&b, "parent", f->parent);
                kv(&b, "implicit", f->implicit ? "1" : "0");
            } else if (kind == 1) {
                const rp_ir_dir_t *d = &ir->dirs[idx[k]];
                rp_buf_puts(&b, "dir ");
                rp_buf_puts(&b, d->id);
                kv(&b, "base", d->base);
                kv(&b, "parent", d->parent);
                rp_buf_puts(&b, " parts=");
                for (size_t j = 0; j < d->part_count; ++j) {
                    if (j) rp_buf_byte(&b, '/');
                    rp_buf_puts(&b, d->parts[j]);
                }
                kv(&b, "feature", d->feature);
            } else {
                const rp_ir_file_t *f = &ir->files[idx[k]];
                rp_buf_puts(&b, "file ");
                rp_buf_puts(&b, f->id);
                kv(&b, "dir", f->dir);
                kv(&b, "source", f->source);
                kv(&b, "name", f->name);
                snprintf(num, sizeof num, "%llu", (unsigned long long)f->size);
                kv(&b, "size", num);
                kv(&b, "vital", f->vital ? "1" : "0");
                kv(&b, "any-arch", f->any_arch ? "1" : "0");
                kv(&b, "feature", f->feature);
                kv(&b, "component-guid", f->component_guid);
            }
            rp_buf_byte(&b, '\n');
        }
    }
    rp_mem_free(alloc, idx);
    return rp_buf_take(&b, out, len);
}
