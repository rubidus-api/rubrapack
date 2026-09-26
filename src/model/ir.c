// src/model/ir.c - `.rpk` AST -> checked package model (include/rubrapack/ir.h, RFC-0002).

#include "rubrapack/buf.h"
#include "rubrapack/ident.h"
#include "rubrapack/ir.h"
#include "rubrapack/mem.h"
#include "rubrapack/pal.h"
#include "rubrapack/pe.h"
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
    size_t                 dir_cap, file_cap;
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
    "Programs", "Desktop", "Startup", "Windows", "System", "Fonts", "Temp", NULL,
};

static bool known_folder(const char *s) {
    for (size_t k = 0; known_folders[k]; ++k) {
        if (strcmp(s, known_folders[k]) == 0) return true;
    }
    return false;
}

// ---- tables ----------------------------------------------------------------------------------

static const char *const top_kinds[] = { "package", "define", "arp", NULL };
static const char *const item_kinds[] = { "feature", "dir", "file", "files", "folder", "property", "action", "registry",
                                          "shortcut", "remove", "copy", "env", "ini", NULL };
static const char *const later_kinds[] = { "service",
                                           "assoc", "protocol", "font", "permission", "require", "search",
                                           "ui", "ui-text", "msix",
                                           "msix-app", "msix-extension", NULL };
static const char *const all_kinds[] = { "package", "define", "arp", "property", "feature", "dir", "file", "files", "folder",
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
    if (strcmp(kind, "action") == 0) return "P3";
    if (strncmp(kind, "ui", 2) == 0) return "P4";
    if (strncmp(kind, "msix", 4) == 0) return "P8";
    return "P3";
}

static rp_ir_dir_t *push_dir(ctx_t *c) {
    rp_ir_t *ir = c->ir;
    if (ir->dir_count == c->dir_cap) {
        size_t cap = c->dir_cap ? c->dir_cap * 2 : 16;
        rp_ir_dir_t *v = rp_mem_alloc(c->alloc, cap, sizeof *v);
        if (v == NULL) {
            c->nomem = true;
            return NULL;
        }
        if (ir->dir_count) memcpy(v, ir->dirs, ir->dir_count * sizeof *v);
        rp_mem_free(c->alloc, ir->dirs);
        ir->dirs = v;
        c->dir_cap = cap;
    }
    rp_ir_dir_t *d = &ir->dirs[ir->dir_count++];
    memset(d, 0, sizeof *d);
    return d;
}

static rp_ir_file_t *push_file(ctx_t *c) {
    rp_ir_t *ir = c->ir;
    if (ir->file_count == c->file_cap) {
        size_t cap = c->file_cap ? c->file_cap * 2 : 16;
        rp_ir_file_t *v = rp_mem_alloc(c->alloc, cap, sizeof *v);
        if (v == NULL) {
            c->nomem = true;
            return NULL;
        }
        if (ir->file_count) memcpy(v, ir->files, ir->file_count * sizeof *v);
        rp_mem_free(c->alloc, ir->files);
        ir->files = v;
        c->file_cap = cap;
    }
    rp_ir_file_t *f = &ir->files[ir->file_count++];
    memset(f, 0, sizeof *f);
    return f;
}

// a.b.c or a.b.c.d with a, b <= 255 and c, d <= 65535 (ProductVersion rules).
static bool parse_version(const char *s, uint16_t parts[4], size_t *count) {
    static const unsigned max[4] = { 255, 255, 65535, 65535 };
    const char *p = s;
    size_t n = 0;
    for (;;) {
        unsigned long v = 0;
        size_t digits = 0;
        while (*p >= '0' && *p <= '9' && digits < 6) v = v * 10 + (unsigned long)(*p++ - '0'), ++digits;
        if (digits == 0 || n >= 4 || v > max[n]) return false;
        parts[n++] = (uint16_t)v;
        if (*p != '.') break;
        ++p;
    }
    *count = n;
    return *p == '\0' && n >= 3;
}

static void parse_package(ctx_t *c, const rp_ttable_t *t) {
    static const char *const keys[] = { "name", "summary-name", "manufacturer", "version", "arch", "upgrade-code",
                                        "upgrade-code-x64", "upgrade-code-arm64", "upgrade-code-x86",
                                        "product-code", "scope", "language", "ui", "license", "icon", "reboot",
                                        "downgrade-message", "compress", "cab", "refuse-upgrade-below",
                                        "refuse-upgrade-message", NULL };
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
    if (ir->version && !parse_version(ir->version, ir->version_parts, &ir->version_count)) {
        ERR(c, key_pos(t, "version"), "RP1308",
            "version '%s' must be a.b.c or a.b.c.d with a, b <= 255 and c, d <= 65535", ir->version);
    }
    // Older versions that must be removed by hand first (RFC-0003 section 9, T1): the upgrade is
    // refused and the message names the removal command.
    ir->refuse_below = get_str(c, t, "refuse-upgrade-below", false, NULL);
    ir->refuse_message = get_str(c, t, "refuse-upgrade-message", false, NULL);
    if (ir->refuse_below) {
        uint16_t below[4] = { 0 };
        size_t bn = 0;
        if (!parse_version(ir->refuse_below, below, &bn)) {
            ERR(c, key_pos(t, "refuse-upgrade-below"), "RP1308", "refuse-upgrade-below '%s' must be a version like 1.2.3",
                ir->refuse_below);
        } else if (ir->version_count >= 3 && memcmp(below, ir->version_parts, 3 * sizeof below[0]) != 0) {
            bool higher = false;
            for (int k = 0; k < 3; ++k) {
                if (below[k] != ir->version_parts[k]) {
                    higher = below[k] > ir->version_parts[k];
                    break;
                }
            }
            if (higher) {
                ERR(c, key_pos(t, "refuse-upgrade-below"), "RP1314",
                    "refuse-upgrade-below %s is above this package's version %s", ir->refuse_below, ir->version);
            }
        }
    } else if (ir->refuse_message) {
        ERR(c, key_pos(t, "refuse-upgrade-message"), "RP1314", "refuse-upgrade-message needs refuse-upgrade-below");
    }

    // The source's arch is its home architecture; --arch may build another one (DECISIONS
    // 2026-09-26 "Upgrade family per architecture").
    char *home = get_str(c, t, "arch", true, NULL);
    char *arch = c->opt->arch ? dup(c, c->opt->arch) : (home ? dup(c, home) : NULL);
    if (arch) {
        if (strcmp(arch, "x64") == 0) ir->arch = RP_ARCH_X64;
        else if (strcmp(arch, "arm64") == 0) ir->arch = RP_ARCH_ARM64;
        else if (strcmp(arch, "x86") == 0) ir->arch = RP_ARCH_X86;
        else ERR(c, key_pos(t, "arch"), "RP1308", "arch must be \"x64\", \"arm64\" or \"x86\" (got '%s')", arch);
    }
    if (home && strcmp(home, "x64") != 0 && strcmp(home, "arm64") != 0 && strcmp(home, "x86") != 0) {
        ERR(c, key_pos(t, "arch"), "RP1308", "arch must be \"x64\", \"arm64\" or \"x86\" (got '%s')", home);
    }

    ir->upgrade_code = get_str(c, t, "upgrade-code", true, NULL);
    if (ir->upgrade_code && !guid_ok(ir->upgrade_code)) {
        ERR(c, key_pos(t, "upgrade-code"), "RP1308", "upgrade-code must be a GUID like {12345678-1234-1234-1234-123456789ABC}");
    }
    // Each architecture is its own upgrade family: a build for another architecture than the
    // source's own needs upgrade-code-<arch>, so one UpgradeCode never spans two architectures.
    char code_key[32];
    snprintf(code_key, sizeof code_key, "upgrade-code-%s", arch ? arch : "");
    char *own = arch ? get_str(c, t, code_key, false, NULL) : NULL;
    if (own && !guid_ok(own)) ERR(c, key_pos(t, code_key), "RP1308", "%s must be a GUID", code_key);
    if (own) {
        if (ir->upgrade_code && strcmp(own, ir->upgrade_code) == 0 && home && arch && strcmp(home, arch) != 0) {
            ERR(c, key_pos(t, code_key), "RP1309", "%s must differ from upgrade-code (one upgrade family per architecture)", code_key);
        }
        rp_mem_free(c->alloc, ir->upgrade_code);
        ir->upgrade_code = own;
    } else if (home && arch && strcmp(home, arch) != 0) {
        ERR(c, key_pos(t, "arch"), "RP1309",
            "building %s from a %s source needs its own upgrade family: add %s = \"{...}\" to [package]", arch, home, code_key);
    }
    for (int k = 0; k < 3; ++k) {       // the others still have to be GUIDs, and distinct
        static const char *const others[] = { "upgrade-code-x64", "upgrade-code-arm64", "upgrade-code-x86" };
        const rp_tkey_t *key = find_key(t, others[k]);
        if (key && key->val.kind == RP_TV_STRING && strcmp(others[k], code_key) != 0) {
            char *g = get_str(c, t, others[k], false, NULL);
            if (g && !guid_ok(g)) ERR(c, key->pos, "RP1308", "%s must be a GUID", others[k]);
            rp_mem_free(c->alloc, g);
        }
    }
    rp_mem_free(c->alloc, home);
    rp_mem_free(c->alloc, arch);
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

// RFC-0002 8.1: relative, '/' only.
static bool source_path_ok(ctx_t *c, const char *s, rp_pos_t pos) {
    if (strchr(s, '\\')) {
        ERR(c, pos, "RP1502", "source paths use '/', not '\\\\' (got '%s')", s);
        return false;
    }
    if (s[0] == '/' || (s[0] && s[1] == ':')) {
        ERR(c, pos, "RP1501", "source path '%s' must be relative to the .rpk file", s);
        return false;
    }
    return true;
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
    if (!source_path_ok(c, s, sp)) return;
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

// ---- [files.*]: wildcards ---------------------------------------------------------------------

// One path segment against one pattern segment: '*' = any run (no '/'), '?' = one character.
static bool seg_match(const char *pat, const char *name) {
    if (*pat == '\0') return *name == '\0';
    if (*pat == '*') {
        for (const char *n = name;; ++n) {
            if (seg_match(pat + 1, n)) return true;
            if (*n == '\0') return false;
        }
    }
    if (*name == '\0') return false;
    if (*pat == '?') {
        size_t step = 1;
        while ((((unsigned char)name[step]) & 0xC0u) == 0x80u) ++step;     // one UTF-8 character
        return seg_match(pat + 1, name + step);
    }
    return *pat == *name && seg_match(pat + 1, name + 1);
}

typedef struct {
    ctx_t       *c;
    char       **segs;
    size_t       nseg;
    const char  *root;          // OS path of the literal prefix
    char       **found;         // relative paths below root
    size_t       count, cap;
    rp_pos_t     pos;
    bool         failed;
} glob_t;

static void glob_add(glob_t *g, const char *rel) {
    for (size_t k = 0; k < g->count; ++k) {
        if (strcmp(g->found[k], rel) == 0) return;          // "**" can reach a file twice
    }
    if (g->count == g->cap) {
        size_t cap = g->cap ? g->cap * 2 : 32;
        char **v = rp_mem_alloc(g->c->alloc, cap, sizeof *v);
        if (v == NULL) {
            g->c->nomem = true;
            return;
        }
        if (g->count) memcpy(v, g->found, g->count * sizeof *v);
        rp_mem_free(g->c->alloc, g->found);
        g->found = v;
        g->cap = cap;
    }
    g->found[g->count++] = dup(g->c, rel);
}

static char *join(ctx_t *c, const char *a, const char *b) {
    if (a == NULL || a[0] == '\0') return dup(c, b);
    size_t n = strlen(a) + strlen(b) + 2;
    char *r = rp_mem_alloc(c->alloc, n, 1);
    if (r == NULL) {
        c->nomem = true;
        return NULL;
    }
    snprintf(r, n, "%s/%s", a, b);
    return r;
}

static void glob_walk(glob_t *g, const char *rel, size_t seg, size_t depth) {
    ctx_t *c = g->c;
    if (g->failed || c->nomem || depth > 64 || g->count >= 100000) return;     // RFC-0001 14.3 limits
    char *dir = rel[0] ? join(c, g->root, rel) : dup(c, g->root);
    char **names = NULL;
    size_t n = 0;
    proven_err_t e = dir ? rp_pal_list_dir(c->alloc, dir, &names, &n) : PROVEN_ERR_NOMEM;
    if (e == PROVEN_ERR_INVALID_ENCODING) {
        ERR(c, g->pos, "RP1506", "a file name under '%s' is not valid Unicode and cannot be packaged", dir);
        g->failed = true;
    }
    if (e != PROVEN_OK) {
        rp_mem_free(c->alloc, dir);
        return;
    }
    // Sorted by bytes, so the result does not depend on the file system's order.
    for (size_t i = 1; i < n; ++i) {
        for (size_t j = i; j > 0 && strcmp(names[j - 1], names[j]) > 0; --j) {
            char *t = names[j];
            names[j] = names[j - 1];
            names[j - 1] = t;
        }
    }
    const char *pat = g->segs[seg];
    bool last = seg + 1 == g->nseg;
    if (strcmp(pat, "**") == 0) {
        if (!last) glob_walk(g, rel, seg + 1, depth + 1);        // zero folders
        for (size_t k = 0; k < n; ++k) {
            char *child = join(c, dir, names[k]);
            rp_fskind_t kind = child ? rp_pal_stat(c->alloc, child, NULL) : RP_FS_NONE;
            char *crel = join(c, rel, names[k]);
            if (kind == RP_FS_DIR && crel) glob_walk(g, crel, seg, depth + 1);
            else if (kind == RP_FS_FILE && last && crel) glob_add(g, crel);
            rp_mem_free(c->alloc, child);
            rp_mem_free(c->alloc, crel);
        }
    } else {
        for (size_t k = 0; k < n; ++k) {
            if (!seg_match(pat, names[k])) continue;
            char *child = join(c, dir, names[k]);
            rp_fskind_t kind = child ? rp_pal_stat(c->alloc, child, NULL) : RP_FS_NONE;
            char *crel = join(c, rel, names[k]);
            if (kind == RP_FS_LINK) {
                ERR(c, g->pos, "RP1504", "'%s' is a symbolic link; links are not followed", child);
                g->failed = true;
            } else if (last && kind == RP_FS_FILE && crel) {
                glob_add(g, crel);
            } else if (!last && kind == RP_FS_DIR && crel) {
                glob_walk(g, crel, seg + 1, depth + 1);
            }
            rp_mem_free(c->alloc, child);
            rp_mem_free(c->alloc, crel);
        }
    }
    for (size_t k = 0; k < n; ++k) rp_mem_free(c->alloc, names[k]);
    rp_mem_free(c->alloc, names);
    rp_mem_free(c->alloc, dir);
}

static const rp_ir_dir_t *find_dir(const rp_ir_t *ir, const char *id);

// Implicit sub folder `name` below dir `parent`, created once.
static const char *implicit_dir(ctx_t *c, const char *parent, const char *name, rp_pos_t pos) {
    char *logical = join(c, parent, name);
    if (logical == NULL) return NULL;
    char key[23];
    rp_key_derive('D', logical, key);
    rp_mem_free(c->alloc, logical);
    const rp_ir_dir_t *have = find_dir(c->ir, key);
    if (have) return have->id;
    rp_ir_dir_t *d = push_dir(c);
    if (d == NULL) return NULL;
    d->id = dup(c, key);
    d->parent = dup(c, parent);
    d->parts = rp_mem_alloc(c->alloc, 1, sizeof *d->parts);
    if (d->parts) {
        d->parts[0] = dup(c, name);
        d->part_count = 1;
    }
    d->implicit = true;
    d->pos = pos;
    target_name_ok(c, name, pos);
    return d->id;
}

static void expand_files(ctx_t *c, const rp_ttable_t *t) {
    static const char *const keys[] = { "dir", "glob", "feature", "keep", "vital", "any-arch", NULL };
    check_keys(c, t, keys);
    check_id(c, t, 72);
    char *dir = get_str(c, t, "dir", true, NULL);
    char *pattern = get_str(c, t, "glob", true, NULL);
    char *feature = get_str(c, t, "feature", false, NULL);
    bool vital = get_bool(c, t, "vital", true), any_arch = get_bool(c, t, "any-arch", false);
    if (get_bool(c, t, "keep", false)) ERR(c, key_pos(t, "keep"), "RP1901", "keep for files is not supported yet (planned for P3)");
    rp_pos_t gp = key_pos(t, "glob");
    glob_t g = { .c = c, .pos = gp };
    if (dir && pattern && source_path_ok(c, pattern, gp)) {
        const rp_ir_dir_t *d = find_dir(c->ir, dir);
        if (d == NULL) ERR(c, key_pos(t, "dir"), "RP1307", "dir '%s' is not defined", dir);
        if (feature == NULL && d && d->feature) feature = dup(c, d->feature);
        // Split into segments; the leading ones without wildcards are the root.
        size_t nseg = 1;
        for (const char *q = pattern; *q; ++q) nseg += *q == '/';
        g.segs = rp_mem_alloc(c->alloc, nseg, sizeof *g.segs);
        char *copy = dup(c, pattern);
        size_t lit = 0;
        bool wild = false;
        for (char *q = copy, *next; g.segs && copy && q; q = next) {
            next = strchr(q, '/');
            if (next) *next++ = '\0';
            g.segs[g.nseg] = dup(c, q);
            bool has = strpbrk(q, "*?") != NULL;
            if (!wild && !has) ++lit;
            wild |= has;
            ++g.nseg;
        }
        if (lit == g.nseg) --lit;           // no wildcard: the last segment is the file itself
        char *root = dup(c, c->opt->source_dir ? c->opt->source_dir : ".");
        for (size_t k = 0; k < lit && root; ++k) {
            char *r = join(c, root, g.segs[k]);
            rp_mem_free(c->alloc, root);
            root = r;
        }
        g.root = root;
        // The source path as written, relative to the .rpk: the literal segments, then rel.
        char *prefix = dup(c, "");
        for (size_t k = 0; k < lit && prefix; ++k) {
            char *r = join(c, prefix, g.segs[k]);
            rp_mem_free(c->alloc, prefix);
            prefix = r;
        }
        char **segs_all = g.segs;
        g.segs += lit;
        g.nseg -= lit;
        if (root && g.nseg > 0) glob_walk(&g, "", 0, 0);
        if (!g.failed && g.count == 0) ERR(c, gp, "RP1503", "glob '%s' matches no file", pattern);
        for (size_t k = 1; k < g.count; ++k) {
            for (size_t j = k; j > 0 && strcmp(g.found[j - 1], g.found[j]) > 0; --j) {
                char *tmp = g.found[j];
                g.found[j] = g.found[j - 1];
                g.found[j - 1] = tmp;
            }
        }
        for (size_t k = 0; k < g.count && !c->nomem; ++k) {
            const char *rel = g.found[k];
            // Sub folders below the wildcard become implicit dirs.
            const char *at = dir;
            char *walk = dup(c, rel);
            char *q = walk;
            for (char *slash; q && (slash = strchr(q, '/')) != NULL; q = slash + 1) {
                *slash = '\0';
                at = implicit_dir(c, at, q, gp);
                if (at == NULL) break;
            }
            char *logical = join(c, t->id, rel);
            char key[23];
            if (logical) rp_key_derive('F', logical, key);
            rp_ir_file_t *f = push_file(c);
            if (f && at && q && logical) {
                f->id = dup(c, key);
                f->dir = dup(c, at);
                f->source = join(c, prefix, rel);
                f->source_path = join(c, g.root, rel);
                f->name = dup(c, q);
                f->vital = vital;
                f->any_arch = any_arch;
                f->feature = feature ? dup(c, feature) : NULL;
                f->pos = gp;
                if (rp_pal_stat(c->alloc, f->source_path, &f->size) != RP_FS_FILE) {
                    ERR(c, gp, "RP1508", "'%s' is not a regular file", f->source);
                }
                if (c->opt->output && rp_pal_same_file(c->alloc, f->source_path, c->opt->output)) {
                    ERR(c, gp, "RP1505", "glob '%s' matches the output file '%s'", pattern, c->opt->output);
                }
                target_name_ok(c, f->name, gp);
            }
            rp_mem_free(c->alloc, logical);
            rp_mem_free(c->alloc, walk);
        }
        for (size_t k = 0; k < g.count; ++k) rp_mem_free(c->alloc, g.found[k]);
        rp_mem_free(c->alloc, g.found);
        for (size_t k = 0; segs_all && k < lit + g.nseg; ++k) rp_mem_free(c->alloc, segs_all[k]);
        rp_mem_free(c->alloc, segs_all);
        rp_mem_free(c->alloc, copy);
        rp_mem_free(c->alloc, root);
        rp_mem_free(c->alloc, prefix);
    }
    rp_mem_free(c->alloc, dir);
    rp_mem_free(c->alloc, pattern);
    rp_mem_free(c->alloc, feature);
}

static void parse_folder(ctx_t *c, const rp_ttable_t *t, rp_ir_folder_t *f) {
    static const char *const keys[] = { "dir", "name", "keep", "feature", NULL };
    check_keys(c, t, keys);
    check_id(c, t, 72);
    f->id = dup(c, t->id);
    f->pos = t->pos;
    f->dir = get_str(c, t, "dir", true, NULL);
    f->name = get_str(c, t, "name", true, NULL);
    f->keep = get_bool(c, t, "keep", false);
    f->feature = get_str(c, t, "feature", false, NULL);
    if (f->name) target_name_ok(c, f->name, key_pos(t, "name"));
}

// Properties the tool writes itself, or that belong to the engine (RFC-0003 1).
static bool tool_property(const char *s) {
    static const char *const names[] = { "ALLUSERS", "REBOOT", "SECURECUSTOMPROPERTIES", "MSIHIDDENPROPERTIES",
                                         "INSTALLLEVEL", "REMOVE", "REINSTALL", "ADDLOCAL", "TARGETDIR", "PRODUCTCODE",
                                         "UPGRADECODE", NULL };
    for (size_t k = 0; names[k]; ++k) {
        if (strcmp(s, names[k]) == 0) return true;
    }
    return strncmp(s, "ARP", 3) == 0 || strncmp(s, "RP_", 3) == 0 || strncmp(s, "MSI", 3) == 0;
}

static void parse_arp(ctx_t *c, const rp_ttable_t *t) {
    static const char *const keys[] = { "no-modify", "no-repair", "help", "about", "icon", NULL };
    check_keys(c, t, keys);
    c->ir->arp_no_modify = get_bool(c, t, "no-modify", false);
    c->ir->arp_no_repair = get_bool(c, t, "no-repair", false);
    c->ir->arp_help = get_str(c, t, "help", false, NULL);
    c->ir->arp_about = get_str(c, t, "about", false, NULL);
    if (find_key(t, "icon")) ERR(c, key_pos(t, "icon"), "RP1901", "icon is not supported yet (planned for P3)");
}

static void parse_property(ctx_t *c, const rp_ttable_t *t, rp_ir_property_t *p) {
    static const char *const keys[] = { "value", "secure", "hidden", NULL };
    check_keys(c, t, keys);
    p->id = dup(c, t->id);
    p->pos = t->pos;
    bool upper = t->id[0] != '\0' && strlen(t->id) <= 72;
    for (const char *s = t->id; *s; ++s) upper &= (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || *s == '_';
    if (!upper || (t->id[0] >= '0' && t->id[0] <= '9')) {
        ERR(c, t->pos, "RP1310", "property '%s' must be a public name: upper-case letters, digits and '_'", t->id);
    } else if (tool_property(t->id)) {
        ERR(c, t->pos, "RP1310", "property '%s' is set by rubrapack or the installer and cannot be defined here", t->id);
    }
    p->value = get_str(c, t, "value", true, NULL);
    p->secure = get_bool(c, t, "secure", false);
    p->hidden = get_bool(c, t, "hidden", false);
}

static void parse_action(ctx_t *c, const rp_ttable_t *t, rp_ir_action_t *a) {
    static const char *const keys[] = { "run", "do", "undo", "check", NULL };
    check_keys(c, t, keys);
    check_id(c, t, 40);            // leaves room for the RP_<ID>_<Suffix> keys (72)
    a->id = dup(c, t->id);
    a->pos = t->pos;
    char *run = get_str(c, t, "run", true, NULL);
    if (run) {
        if (strncmp(run, "file:", 5) != 0 || run[5] == '\0') {
            ERR(c, key_pos(t, "run"), "RP1311", "run must be \"file:<ID>\" naming a [file.*] of this package");
        } else {
            a->run_file = dup(c, run + 5);
        }
        rp_mem_free(c->alloc, run);
    }
    bool has_do = false, has_undo = false;
    a->do_args = get_str(c, t, "do", false, &has_do);
    a->undo_args = get_str(c, t, "undo", false, &has_undo);
    a->check_args = get_str(c, t, "check", false, NULL);
    if (!has_do || !has_undo) {
        ERR(c, t->pos, "RP1312", "[action.%s] needs both do and undo (the undo also rolls back a failed do)", t->id);
    }
}

static void parse_registry(ctx_t *c, const rp_ttable_t *t, rp_ir_registry_t *r) {
    static const char *const keys[] = { "root", "key", "name", "value", "type", "remove", "keep", "view", "with",
                                        "feature", NULL };
    check_keys(c, t, keys);
    check_id(c, t, 72);
    r->id = dup(c, t->id);
    r->pos = t->pos;
    char *root = get_str(c, t, "root", true, NULL);
    if (root) {
        if (strcmp(root, "HKLM") == 0) r->root = RP_ROOT_HKLM;
        else if (strcmp(root, "HKCR") == 0) r->root = RP_ROOT_HKCR;
        else if (strcmp(root, "HKCU") == 0) {
            ERR(c, key_pos(t, "root"), "RP1901", "HKCU needs a per-user package, which is not supported yet (planned for P3)");
        } else {
            ERR(c, key_pos(t, "root"), "RP1316", "root must be \"HKLM\", \"HKCR\" or \"HKCU\" (got '%s')", root);
        }
        rp_mem_free(c->alloc, root);
    }
    r->key = get_str(c, t, "key", true, NULL);
    if (r->key && (r->key[0] == '\\' || r->key[strlen(r->key) - 1] == '\\' || strstr(r->key, "\\\\") || has_control(r->key))) {
        ERR(c, key_pos(t, "key"), "RP1316", "key '%s' must not start or end with '\\' or contain an empty part", r->key);
    }
    r->name = get_str(c, t, "name", false, NULL);
    if (r->name && r->name[0] == '\0') {        // "" is the default value, as omitting it
        rp_mem_free(c->alloc, r->name);
        r->name = NULL;
    }
    r->remove = get_bool(c, t, "remove", false);
    r->keep = get_bool(c, t, "keep", false);
    char *view = get_str(c, t, "view", false, NULL);
    if (view) {
        if (strcmp(view, "32") == 0 && c->ir->arch != RP_ARCH_X86) r->view32 = true;
        else if (strcmp(view, "32") != 0 && strcmp(view, "64") != 0) {
            ERR(c, key_pos(t, "view"), "RP1316", "view must be \"32\" or \"64\"");
        } else if (strcmp(view, "64") == 0 && c->ir->arch == RP_ARCH_X86) {
            ERR(c, key_pos(t, "view"), "RP1316", "an x86 package writes the 32-bit registry view only");
        }
        rp_mem_free(c->alloc, view);
    }
    char *with = get_str(c, t, "with", false, NULL);
    if (with) {
        if (strncmp(with, "file:", 5) != 0 || with[5] == '\0') ERR(c, key_pos(t, "with"), "RP1315", "with must be \"file:<ID>\"");
        else r->with_file = dup(c, with + 5);
        rp_mem_free(c->alloc, with);
    }
    r->feature = get_str(c, t, "feature", false, NULL);
    if (r->with_file && r->feature) ERR(c, key_pos(t, "feature"), "RP1316", "a value that goes 'with' a file takes that file's feature");

    char *type = get_str(c, t, "type", false, NULL);
    static const char *const types[] = { "string", "expand", "dword", "binary", "multi" };
    r->type = RP_REG_STRING;
    if (type) {
        bool known = false;
        for (int k = 0; k < 5; ++k) {
            if (strcmp(type, types[k]) == 0) r->type = (rp_reg_type_t)k, known = true;
        }
        if (!known) {
            ERR(c, key_pos(t, "type"), strcmp(type, "qword") == 0 ? "RP1901" : "RP1316",
                strcmp(type, "qword") == 0 ? "qword needs the helper action, which is not supported yet (planned for P3)"
                                           : "type must be string, expand, dword, binary or multi (got '%s')", type);
        }
        rp_mem_free(c->alloc, type);
    }
    const rp_tkey_t *v = find_key(t, "value");
    if (v == NULL) {
        if (!r->remove) ERR(c, t->pos, "RP1202", "[registry.%s] needs 'value' (or remove = true)", t->id);
        return;
    }
    if (r->remove) {
        ERR(c, v->pos, "RP1316", "remove = true removes the value at install; it takes no 'value'");
        return;
    }
    char num[24];
    switch (r->type) {
    case RP_REG_DWORD:
        if (v->val.kind != RP_TV_INT || v->val.i < 0 || v->val.i > 0xFFFFFFFFll) {
            ERR(c, v->pos, "RP1316", "a dword value is an integer 0..4294967295 (0x0..0xFFFFFFFF)");
            return;
        }
        snprintf(num, sizeof num, "%lld", (long long)v->val.i);
        r->value = dup(c, num);
        return;
    case RP_REG_MULTI:
        if (v->val.kind != RP_TV_ARRAY || v->val.count == 0) {
            ERR(c, v->pos, "RP1316", "a multi value is a non-empty array of strings");
            return;
        }
        r->items = rp_mem_alloc(c->alloc, v->val.count, sizeof *r->items);
        if (r->items == NULL) {
            c->nomem = true;
            return;
        }
        for (size_t k = 0; k < v->val.count; ++k) {
            if (v->val.items[k].kind != RP_TV_STRING) {
                ERR(c, v->pos, "RP1316", "a multi value is an array of strings");
                break;
            }
            r->items[r->item_count++] = subst(c, &v->val.items[k]);
        }
        return;
    default:
        break;
    }
    r->value = get_str(c, t, "value", false, NULL);
    if (r->value && (r->type == RP_REG_STRING || r->type == RP_REG_EXPAND) && strstr(r->value, "[~]")) {
        ERR(c, v->pos, "RP1316", "'[~]' makes Windows Installer write a multi-string; use type = \"multi\" and an array");
    }
    if (r->value && r->type == RP_REG_BINARY) {
        size_t n = strlen(r->value);
        bool ok = n > 0 && n % 2 == 0;
        for (size_t k = 0; ok && k < n; ++k) ok = is_hex(r->value[k]);
        if (!ok) ERR(c, v->pos, "RP1316", "a binary value is an even number of hex digits, like \"01A0FF\"");
        for (size_t k = 0; ok && k < n; ++k) {
            if (r->value[k] >= 'a' && r->value[k] <= 'f') r->value[k] = (char)(r->value[k] - 32);
        }
    }
}

static bool shortcut_folder(const char *s) {
    return strcmp(s, "Programs") == 0 || strcmp(s, "Desktop") == 0 || strcmp(s, "StartMenu") == 0 || strcmp(s, "Startup") == 0;
}

static void parse_shortcut(ctx_t *c, const rp_ttable_t *t, rp_ir_shortcut_t *s) {
    static const char *const keys[] = { "dir", "name", "target", "args", "description", "working-dir", NULL };
    check_keys(c, t, keys);
    check_id(c, t, 72);
    s->id = dup(c, t->id);
    s->pos = t->pos;
    s->dir = get_str(c, t, "dir", true, NULL);
    s->name = get_str(c, t, "name", true, NULL);
    if (s->name) target_name_ok(c, s->name, key_pos(t, "name"));
    char *target = get_str(c, t, "target", true, NULL);
    if (target) {
        if (strncmp(target, "file:", 5) != 0 || target[5] == '\0') {
            ERR(c, key_pos(t, "target"), "RP1315", "target must be \"file:<ID>\" naming a [file.*] of this package");
        } else {
            s->target_file = dup(c, target + 5);
        }
        rp_mem_free(c->alloc, target);
    }
    s->args = get_str(c, t, "args", false, NULL);
    s->description = get_str(c, t, "description", false, NULL);
    s->working_dir = get_str(c, t, "working-dir", false, NULL);
}

// A file name pattern: a target name that may contain * and ?.
static bool pattern_ok(ctx_t *c, const char *name, rp_pos_t pos) {
    size_t n = strlen(name);
    char *probe = rp_mem_alloc(c->alloc, n + 1, 1);
    if (probe == NULL) {
        c->nomem = true;
        return false;
    }
    for (size_t k = 0; k <= n; ++k) probe[k] = name[k] == '*' || name[k] == '?' ? 'x' : name[k];
    bool ok = target_name_ok(c, probe, pos);
    rp_mem_free(c->alloc, probe);
    return ok;
}

static void parse_remove(ctx_t *c, const rp_ttable_t *t, rp_ir_remove_t *r) {
    static const char *const keys[] = { "dir", "name", "on", "feature", NULL };
    check_keys(c, t, keys);
    check_id(c, t, 72);
    r->id = dup(c, t->id);
    r->pos = t->pos;
    r->dir = get_str(c, t, "dir", true, NULL);
    r->name = get_str(c, t, "name", false, NULL);
    if (r->name) pattern_ok(c, r->name, key_pos(t, "name"));
    char *on = get_str(c, t, "on", true, NULL);
    if (on) {
        if (strcmp(on, "install") == 0) r->mode = 1;
        else if (strcmp(on, "uninstall") == 0) r->mode = 2;
        else if (strcmp(on, "both") == 0) r->mode = 3;
        else ERR(c, key_pos(t, "on"), "RP1316", "on must be \"install\", \"uninstall\" or \"both\"");
        rp_mem_free(c->alloc, on);
    }
    r->feature = get_str(c, t, "feature", false, NULL);
}

static void parse_copy(ctx_t *c, const rp_ttable_t *t, rp_ir_copy_t *cp) {
    static const char *const keys[] = { "source", "dir", "name", NULL };
    check_keys(c, t, keys);
    check_id(c, t, 72);
    cp->id = dup(c, t->id);
    cp->pos = t->pos;
    char *src = get_str(c, t, "source", true, NULL);
    if (src) {
        if (strncmp(src, "file:", 5) != 0 || src[5] == '\0') ERR(c, key_pos(t, "source"), "RP1315", "source must be \"file:<ID>\"");
        else cp->source_file = dup(c, src + 5);
        rp_mem_free(c->alloc, src);
    }
    cp->dir = get_str(c, t, "dir", true, NULL);
    cp->name = get_str(c, t, "name", false, NULL);
    if (cp->name) target_name_ok(c, cp->name, key_pos(t, "name"));
}

static void parse_ini(ctx_t *c, const rp_ttable_t *t, rp_ir_ini_t *x) {
    static const char *const keys[] = { "dir", "file", "section", "key", "value", "mode", "feature", NULL };
    check_keys(c, t, keys);
    check_id(c, t, 72);
    x->id = dup(c, t->id);
    x->pos = t->pos;
    x->dir = get_str(c, t, "dir", true, NULL);
    x->file = get_str(c, t, "file", true, NULL);
    if (x->file) target_name_ok(c, x->file, key_pos(t, "file"));
    x->section = get_str(c, t, "section", true, NULL);
    x->key = get_str(c, t, "key", true, NULL);
    if ((x->section && (strchr(x->section, ']') || has_control(x->section))) || (x->key && (strchr(x->key, '=') || has_control(x->key)))) {
        ERR(c, t->pos, "RP1316", "an INI section may not contain ']' and a key may not contain '='");
    }
    char *mode = get_str(c, t, "mode", false, NULL);
    if (mode) {
        if (strcmp(mode, "set") == 0) x->mode = 0;
        else if (strcmp(mode, "add") == 0) x->mode = 1;
        else if (strcmp(mode, "remove") == 0) x->mode = 2;
        else ERR(c, key_pos(t, "mode"), "RP1316", "mode must be \"set\", \"add\" or \"remove\"");
        rp_mem_free(c->alloc, mode);
    }
    bool has_value = false;
    x->value = get_str(c, t, "value", false, &has_value);
    if (x->mode != 2 && !has_value) ERR(c, t->pos, "RP1202", "[ini.%s] needs 'value'", t->id);
    if (x->mode == 2 && has_value) ERR(c, key_pos(t, "value"), "RP1316", "mode = \"remove\" removes the key; it takes no 'value'");
    x->feature = get_str(c, t, "feature", false, NULL);
}

static void parse_env(ctx_t *c, const rp_ttable_t *t, rp_ir_env_t *e) {
    static const char *const keys[] = { "name", "value", "mode", "keep", "feature", NULL };
    check_keys(c, t, keys);
    check_id(c, t, 72);
    e->id = dup(c, t->id);
    e->pos = t->pos;
    e->name = get_str(c, t, "name", true, NULL);
    if (e->name && (strchr("=+-!*", e->name[0]) || strchr(e->name, '=') || has_control(e->name))) {
        ERR(c, key_pos(t, "name"), "RP1316", "variable name '%s' may not contain '=' or start with = + - ! *", e->name);
    }
    e->value = get_str(c, t, "value", true, NULL);
    if (e->value && strstr(e->value, "[~]")) {
        ERR(c, key_pos(t, "value"), "RP1316", "write the value without '[~]'; mode = \"append\" or \"prepend\" adds it");
    }
    char *mode = get_str(c, t, "mode", false, NULL);
    if (mode) {
        if (strcmp(mode, "set") == 0) e->mode = 0;
        else if (strcmp(mode, "append") == 0) e->mode = 1;
        else if (strcmp(mode, "prepend") == 0) e->mode = 2;
        else ERR(c, key_pos(t, "mode"), "RP1316", "mode must be \"set\", \"append\" or \"prepend\"");
        rp_mem_free(c->alloc, mode);
    }
    e->keep = get_bool(c, t, "keep", false);
    e->feature = get_str(c, t, "feature", false, NULL);
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
    size_t n = ir->dir_count + ir->file_count + ir->feature_count + ir->folder_count + ir->registry_count +
               ir->shortcut_count + ir->remove_count + ir->copy_count + ir->env_count + ir->ini_count;
    idpos_t *ids = rp_mem_alloc(c->alloc, n, sizeof *ids);
    if (ids == NULL) {
        c->nomem = true;
        return;
    }
    size_t m = 0;
    for (size_t k = 0; k < ir->dir_count; ++k) ids[m++] = (idpos_t){ ir->dirs[k].id, ir->dirs[k].pos };
    for (size_t k = 0; k < ir->file_count; ++k) ids[m++] = (idpos_t){ ir->files[k].id, ir->files[k].pos };
    for (size_t k = 0; k < ir->folder_count; ++k) ids[m++] = (idpos_t){ ir->folders[k].id, ir->folders[k].pos };
    for (size_t k = 0; k < ir->registry_count; ++k) ids[m++] = (idpos_t){ ir->registries[k].id, ir->registries[k].pos };
    for (size_t k = 0; k < ir->shortcut_count; ++k) ids[m++] = (idpos_t){ ir->shortcuts[k].id, ir->shortcuts[k].pos };
    for (size_t k = 0; k < ir->remove_count; ++k) ids[m++] = (idpos_t){ ir->removes[k].id, ir->removes[k].pos };
    for (size_t k = 0; k < ir->copy_count; ++k) ids[m++] = (idpos_t){ ir->copies[k].id, ir->copies[k].pos };
    for (size_t k = 0; k < ir->env_count; ++k) ids[m++] = (idpos_t){ ir->envs[k].id, ir->envs[k].pos };
    for (size_t k = 0; k < ir->ini_count; ++k) ids[m++] = (idpos_t){ ir->inis[k].id, ir->inis[k].pos };
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
    size_t np = ir->dir_count + ir->file_count + ir->folder_count;
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
        // PE files: the machine type must match the package architecture unless any-arch
        // (RFC-0001 9.2); a malformed PE is refused.
        if (f->source_path && f->size >= 2) {
            uint8_t *data;
            size_t len;
            if (rp_pal_read_file(c->alloc, f->source_path, (size_t)f->size, &data, &len) == PROVEN_OK) {
                rp_pe_info_t pi;
                proven_err_t pe_err = rp_pe_read(data, len, &pi);
                if (pe_err == PROVEN_OK && pi.is_pe) {
                    f->pe_machine = pi.machine;
                    f->pe_is_dll = pi.is_dll;
                }
                if (pe_err != PROVEN_OK) {
                    ERR(c, f->pos, "RP1513", "'%s' looks like a program file (PE) but its headers or resources are damaged",
                        f->source);
                } else if (pi.is_pe && !f->any_arch) {
                    uint16_t want = ir->arch == RP_ARCH_X64 ? RP_PE_AMD64 : ir->arch == RP_ARCH_ARM64 ? RP_PE_ARM64 : RP_PE_I386;
                    if (pi.machine != want) {
                        ERR(c, f->pos, "RP1512",
                            "'%s' is built for machine 0x%04X, not for this package's arch; set any-arch = true if that is intended",
                            f->source, pi.machine);
                    }
                }
                rp_mem_free(c->alloc, data);
            }
        }
    }
    for (size_t k = 0; k < ir->folder_count; ++k) {
        rp_ir_folder_t *f = &ir->folders[k];
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
            else ERR(c, f->pos, "RP1202", "folder '%s' needs a feature: set 'feature' here or on its dir", f->id);
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
    }
    // Properties: not a dir/file/feature/folder ID (every directory is a property too).
    for (size_t k = 0; k < ir->property_count; ++k) {
        const rp_ir_property_t *p = &ir->properties[k];
        if (find_dir(ir, p->id) || find_feature(ir, p->id)) {
            ERR(c, p->pos, "RP1310", "property '%s' has the name of a dir or feature", p->id);
        }
        for (size_t j = 0; j < k; ++j) {
            if (strcmp(ir->properties[j].id, p->id) == 0) {
                ERR(c, p->pos, "RP1301", "property '%s' is already defined (line %u)", p->id, (unsigned)ir->properties[j].pos.line);
            }
        }
    }
    // Registry values: `with` names a file; otherwise the feature resolves like a file's (G2).
    for (size_t k = 0; k < ir->registry_count; ++k) {
        rp_ir_registry_t *r = &ir->registries[k];
        if (r->with_file) {
            bool found = false;
            for (size_t j = 0; j < ir->file_count; ++j) found |= strcmp(ir->files[j].id, r->with_file) == 0;
            if (!found) ERR(c, r->pos, "RP1315", "with: file '%s' is not a [file.*] of this package", r->with_file);
            if (r->view32 != (ir->arch == RP_ARCH_X86)) {
                // a component has one bitness: a 32-bit view value cannot share a 64-bit file's component
                if (r->view32) ERR(c, r->pos, "RP1316", "view = \"32\" cannot go 'with' a file of a 64-bit package");
            }
        } else if (r->feature && !find_feature(ir, r->feature)) {
            ERR(c, r->pos, "RP1307", "feature '%s' is not defined", r->feature);
        } else if (r->feature == NULL) {
            if (!declared) r->feature = dup(c, "Main");
            else ERR(c, r->pos, "RP1202", "registry value '%s' needs a feature", r->id);
        }
    }
    // Removals and copies: folders and files exist; a removal's feature resolves like a file's.
    for (size_t k = 0; k < ir->remove_count; ++k) {
        rp_ir_remove_t *r = &ir->removes[k];
        const rp_ir_dir_t *d = r->dir ? find_dir(ir, r->dir) : NULL;
        if (r->dir && d == NULL) {
            ERR(c, r->pos, "RP1315", "dir '%s' is not a dir ID", r->dir);
            continue;
        }
        if (r->feature && !find_feature(ir, r->feature)) {
            ERR(c, r->pos, "RP1307", "feature '%s' is not defined", r->feature);
        } else if (r->feature == NULL) {
            if (d && d->feature) r->feature = dup(c, d->feature);
            else if (!declared) r->feature = dup(c, "Main");
            else ERR(c, r->pos, "RP1202", "[remove.%s] needs a feature: set 'feature' here or on its dir", r->id);
        }
    }
    for (size_t k = 0; k < ir->ini_count; ++k) {
        rp_ir_ini_t *x = &ir->inis[k];
        const rp_ir_dir_t *d = x->dir ? find_dir(ir, x->dir) : NULL;
        if (x->dir && d == NULL) {
            ERR(c, x->pos, "RP1315", "dir '%s' is not a dir ID", x->dir);
            continue;
        }
        if (x->feature && !find_feature(ir, x->feature)) {
            ERR(c, x->pos, "RP1307", "feature '%s' is not defined", x->feature);
        } else if (x->feature == NULL) {
            if (d && d->feature) x->feature = dup(c, d->feature);
            else if (!declared) x->feature = dup(c, "Main");
            else ERR(c, x->pos, "RP1202", "[ini.%s] needs a feature: set 'feature' here or on its dir", x->id);
        }
    }
    for (size_t k = 0; k < ir->env_count; ++k) {
        rp_ir_env_t *e = &ir->envs[k];
        if (e->feature && !find_feature(ir, e->feature)) {
            ERR(c, e->pos, "RP1307", "feature '%s' is not defined", e->feature);
        } else if (e->feature == NULL) {
            if (!declared) e->feature = dup(c, "Main");
            else ERR(c, e->pos, "RP1202", "[env.%s] needs a feature", e->id);
        }
    }
    for (size_t k = 0; k < ir->copy_count; ++k) {
        const rp_ir_copy_t *cp = &ir->copies[k];
        bool found = false;
        for (size_t j = 0; cp->source_file && j < ir->file_count; ++j) found |= strcmp(ir->files[j].id, cp->source_file) == 0;
        if (cp->source_file && !found) ERR(c, cp->pos, "RP1315", "source: file '%s' is not a [file.*] of this package", cp->source_file);
        if (cp->dir && !find_dir(ir, cp->dir)) ERR(c, cp->pos, "RP1315", "dir '%s' is not a dir ID", cp->dir);
    }
    // Shortcuts: target file and folders exist.
    for (size_t k = 0; k < ir->shortcut_count; ++k) {
        const rp_ir_shortcut_t *sc = &ir->shortcuts[k];
        bool found = false;
        for (size_t j = 0; sc->target_file && j < ir->file_count; ++j) found |= strcmp(ir->files[j].id, sc->target_file) == 0;
        if (sc->target_file && !found) ERR(c, sc->pos, "RP1315", "target: file '%s' is not a [file.*] of this package", sc->target_file);
        if (sc->dir && !shortcut_folder(sc->dir) && !find_dir(ir, sc->dir)) {
            ERR(c, sc->pos, "RP1315", "dir '%s' is neither a dir ID nor Programs, Desktop, StartMenu or Startup", sc->dir);
        }
        if (sc->working_dir && !find_dir(ir, sc->working_dir)) {
            ERR(c, sc->pos, "RP1315", "working-dir '%s' is not a dir ID", sc->working_dir);
        }
    }
    // Actions: run names an exe of this package that the package's machines can start.
    for (size_t k = 0; k < ir->action_count; ++k) {
        const rp_ir_action_t *a = &ir->actions[k];
        if (a->run_file == NULL) continue;
        const rp_ir_file_t *f = NULL;
        for (size_t j = 0; j < ir->file_count; ++j) {
            if (strcmp(ir->files[j].id, a->run_file) == 0) f = &ir->files[j];
        }
        if (f == NULL) {
            ERR(c, a->pos, "RP1311", "run: file '%s' is not a [file.*] of this package", a->run_file);
            continue;
        }
        bool runs = f->pe_machine == RP_PE_I386 ||
                    (f->pe_machine == RP_PE_AMD64 && ir->arch != RP_ARCH_X86) ||
                    (f->pe_machine == RP_PE_ARM64 && ir->arch == RP_ARCH_ARM64);
        if (f->pe_machine == 0 || f->pe_is_dll) {
            ERR(c, a->pos, "RP1311", "run: '%s' is not a program (.exe)", f->source ? f->source : f->id);
        } else if (!runs) {
            ERR(c, a->pos, "RP1311", "run: '%s' (machine 0x%04X) cannot run on this package's machines", f->source, f->pe_machine);
        }
        for (size_t j = 0; j < k; ++j) {
            if (strcmp(ir->actions[j].id, a->id) == 0) ERR(c, a->pos, "RP1301", "action '%s' is already defined", a->id);
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

static int cmp_str(const char *a, const char *b) { return strcmp(a ? a : "", b ? b : ""); }

proven_err_t rp_ir_build(proven_allocator_t alloc, const rp_tdoc_t *doc, const rp_ir_options_t *opt, rp_ir_t *ir,
                         rp_srcdiags_t *diags) {
    if (doc == NULL || opt == NULL || ir == NULL || diags == NULL) return PROVEN_ERR_INVALID_ARG;
    memset(ir, 0, sizeof *ir);
    ir->alloc = alloc;
    ctx_t c = { .alloc = alloc, .doc = doc, .opt = opt, .d = diags, .ir = ir };

    const rp_ttable_t *package = NULL;
    size_t nfeat = 0, ndir = 0, nfile = 0, nfolder = 0, nprop = 0, naction = 0, nreg = 0, nshort = 0, nrem = 0, ncopy = 0, nenv = 0, nini = 0;
    const rp_ttable_t *arp = NULL;
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
        else if (strcmp(t->kind, "folder") == 0) ++nfolder;
        else if (strcmp(t->kind, "property") == 0) ++nprop;
        else if (strcmp(t->kind, "action") == 0) ++naction;
        else if (strcmp(t->kind, "registry") == 0) ++nreg;
        else if (strcmp(t->kind, "shortcut") == 0) ++nshort;
        else if (strcmp(t->kind, "remove") == 0) ++nrem;
        else if (strcmp(t->kind, "copy") == 0) ++ncopy;
        else if (strcmp(t->kind, "env") == 0) ++nenv;
        else if (strcmp(t->kind, "ini") == 0) ++nini;
        else if (strcmp(t->kind, "arp") == 0) arp = t;
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
    if (arp) parse_arp(&c, arp);
    if (naction > 50) {         // the Undo pairs fill 3400..3499 (RFC-0003 2)
        rp_pos_t top = { 1, 1 };
        ERR(&c, top, "RP1313", "at most 50 [action.*] tables (this file has %zu)", naction);
    }

    ir->features = rp_mem_alloc(alloc, nfeat + 1, sizeof *ir->features);
    ir->folders = rp_mem_alloc(alloc, nfolder, sizeof *ir->folders);
    ir->properties = rp_mem_alloc(alloc, nprop + 1, sizeof *ir->properties);
    ir->actions = rp_mem_alloc(alloc, naction + 1, sizeof *ir->actions);
    ir->registries = rp_mem_alloc(alloc, nreg + 1, sizeof *ir->registries);
    ir->shortcuts = rp_mem_alloc(alloc, nshort + 1, sizeof *ir->shortcuts);
    ir->removes = rp_mem_alloc(alloc, nrem + 1, sizeof *ir->removes);
    ir->copies = rp_mem_alloc(alloc, ncopy + 1, sizeof *ir->copies);
    ir->envs = rp_mem_alloc(alloc, nenv + 1, sizeof *ir->envs);
    ir->inis = rp_mem_alloc(alloc, nini + 1, sizeof *ir->inis);
    if (ir->properties == NULL || ir->actions == NULL || ir->registries == NULL || ir->shortcuts == NULL ||
        ir->removes == NULL || ir->copies == NULL || ir->envs == NULL || ir->inis == NULL) {
        c.nomem = true;
    }
    (void)ndir;
    (void)nfile;
    if (ir->features == NULL || ir->folders == NULL) c.nomem = true;
    for (size_t k = 0; k < doc->count && !c.nomem; ++k) {
        const rp_ttable_t *t = &doc->tables[k];
        if (t->id == NULL) continue;
        if (strcmp(t->kind, "feature") == 0) {
            rp_ir_feature_t *f = &ir->features[ir->feature_count++];
            memset(f, 0, sizeof *f);
            parse_feature(&c, t, f);
        } else if (strcmp(t->kind, "dir") == 0) {
            rp_ir_dir_t *d = push_dir(&c);
            if (d) parse_dir(&c, t, d);
        } else if (strcmp(t->kind, "file") == 0) {
            rp_ir_file_t *f = push_file(&c);
            if (f) parse_file(&c, t, f);
        } else if (strcmp(t->kind, "folder") == 0) {
            rp_ir_folder_t *f = &ir->folders[ir->folder_count++];
            memset(f, 0, sizeof *f);
            parse_folder(&c, t, f);
        } else if (strcmp(t->kind, "property") == 0) {
            rp_ir_property_t *p = &ir->properties[ir->property_count++];
            memset(p, 0, sizeof *p);
            parse_property(&c, t, p);
        } else if (strcmp(t->kind, "action") == 0) {
            rp_ir_action_t *a = &ir->actions[ir->action_count++];
            memset(a, 0, sizeof *a);
            parse_action(&c, t, a);
        } else if (strcmp(t->kind, "registry") == 0) {
            rp_ir_registry_t *r = &ir->registries[ir->registry_count++];
            memset(r, 0, sizeof *r);
            parse_registry(&c, t, r);
        } else if (strcmp(t->kind, "shortcut") == 0) {
            rp_ir_shortcut_t *sc = &ir->shortcuts[ir->shortcut_count++];
            memset(sc, 0, sizeof *sc);
            parse_shortcut(&c, t, sc);
        } else if (strcmp(t->kind, "remove") == 0) {
            rp_ir_remove_t *r = &ir->removes[ir->remove_count++];
            memset(r, 0, sizeof *r);
            parse_remove(&c, t, r);
        } else if (strcmp(t->kind, "copy") == 0) {
            rp_ir_copy_t *cp = &ir->copies[ir->copy_count++];
            memset(cp, 0, sizeof *cp);
            parse_copy(&c, t, cp);
        } else if (strcmp(t->kind, "env") == 0) {
            rp_ir_env_t *e = &ir->envs[ir->env_count++];
            memset(e, 0, sizeof *e);
            parse_env(&c, t, e);
        } else if (strcmp(t->kind, "ini") == 0) {
            rp_ir_ini_t *x = &ir->inis[ir->ini_count++];
            memset(x, 0, sizeof *x);
            parse_ini(&c, t, x);
        }
    }
    // Wildcards after every dir is known (their feature and the implicit sub folders).
    for (size_t k = 0; k < doc->count && !c.nomem; ++k) {
        const rp_ttable_t *t = &doc->tables[k];
        if (t->id && strcmp(t->kind, "files") == 0) expand_files(&c, t);
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
    // Properties and actions in ID order: the output never depends on the order of tables.
    for (size_t i = 1; !c.nomem && i < ir->property_count; ++i) {
        for (size_t j = i; j > 0 && cmp_str(ir->properties[j - 1].id, ir->properties[j].id) > 0; --j) {
            rp_ir_property_t t = ir->properties[j];
            ir->properties[j] = ir->properties[j - 1];
            ir->properties[j - 1] = t;
        }
    }
    for (size_t i = 1; !c.nomem && i < ir->action_count; ++i) {
        for (size_t j = i; j > 0 && cmp_str(ir->actions[j - 1].id, ir->actions[j].id) > 0; --j) {
            rp_ir_action_t t = ir->actions[j];
            ir->actions[j] = ir->actions[j - 1];
            ir->actions[j - 1] = t;
        }
    }
    for (size_t i = 1; !c.nomem && i < ir->ini_count; ++i) {
        for (size_t j = i; j > 0 && cmp_str(ir->inis[j - 1].id, ir->inis[j].id) > 0; --j) {
            rp_ir_ini_t t = ir->inis[j];
            ir->inis[j] = ir->inis[j - 1];
            ir->inis[j - 1] = t;
        }
    }
    for (size_t i = 1; !c.nomem && i < ir->env_count; ++i) {
        for (size_t j = i; j > 0 && cmp_str(ir->envs[j - 1].id, ir->envs[j].id) > 0; --j) {
            rp_ir_env_t t = ir->envs[j];
            ir->envs[j] = ir->envs[j - 1];
            ir->envs[j - 1] = t;
        }
    }
    for (size_t i = 1; !c.nomem && i < ir->remove_count; ++i) {
        for (size_t j = i; j > 0 && cmp_str(ir->removes[j - 1].id, ir->removes[j].id) > 0; --j) {
            rp_ir_remove_t t = ir->removes[j];
            ir->removes[j] = ir->removes[j - 1];
            ir->removes[j - 1] = t;
        }
    }
    for (size_t i = 1; !c.nomem && i < ir->copy_count; ++i) {
        for (size_t j = i; j > 0 && cmp_str(ir->copies[j - 1].id, ir->copies[j].id) > 0; --j) {
            rp_ir_copy_t t = ir->copies[j];
            ir->copies[j] = ir->copies[j - 1];
            ir->copies[j - 1] = t;
        }
    }
    for (size_t i = 1; !c.nomem && i < ir->shortcut_count; ++i) {
        for (size_t j = i; j > 0 && cmp_str(ir->shortcuts[j - 1].id, ir->shortcuts[j].id) > 0; --j) {
            rp_ir_shortcut_t t = ir->shortcuts[j];
            ir->shortcuts[j] = ir->shortcuts[j - 1];
            ir->shortcuts[j - 1] = t;
        }
    }
    for (size_t i = 1; !c.nomem && i < ir->registry_count; ++i) {
        for (size_t j = i; j > 0 && cmp_str(ir->registries[j - 1].id, ir->registries[j].id) > 0; --j) {
            rp_ir_registry_t t = ir->registries[j];
            ir->registries[j] = ir->registries[j - 1];
            ir->registries[j - 1] = t;
        }
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
                     ir->downgrade_message, ir->refuse_below, ir->refuse_message };
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
    for (size_t k = 0; k < ir->folder_count; ++k) {
        rp_ir_folder_t *f = &ir->folders[k];
        rp_mem_free(a, f->id);
        rp_mem_free(a, f->dir);
        rp_mem_free(a, f->name);
        rp_mem_free(a, f->feature);
    }
    for (size_t k = 0; k < ir->property_count; ++k) {
        rp_mem_free(a, ir->properties[k].id);
        rp_mem_free(a, ir->properties[k].value);
    }
    for (size_t k = 0; k < ir->action_count; ++k) {
        rp_ir_action_t *x = &ir->actions[k];
        char *xs[] = { x->id, x->run_file, x->do_args, x->undo_args, x->check_args };
        for (size_t j = 0; j < sizeof xs / sizeof xs[0]; ++j) rp_mem_free(a, xs[j]);
    }
    for (size_t k = 0; k < ir->registry_count; ++k) {
        rp_ir_registry_t *r = &ir->registries[k];
        char *rs[] = { r->id, r->key, r->name, r->value, r->with_file, r->feature };
        for (size_t j = 0; j < sizeof rs / sizeof rs[0]; ++j) rp_mem_free(a, rs[j]);
        for (size_t j = 0; j < r->item_count; ++j) rp_mem_free(a, r->items[j]);
        rp_mem_free(a, r->items);
    }
    rp_mem_free(a, ir->registries);
    for (size_t k = 0; k < ir->shortcut_count; ++k) {
        rp_ir_shortcut_t *sc = &ir->shortcuts[k];
        char *ss[] = { sc->id, sc->dir, sc->name, sc->target_file, sc->args, sc->description, sc->working_dir };
        for (size_t j = 0; j < sizeof ss / sizeof ss[0]; ++j) rp_mem_free(a, ss[j]);
    }
    rp_mem_free(a, ir->shortcuts);
    for (size_t k = 0; k < ir->remove_count; ++k) {
        rp_ir_remove_t *r = &ir->removes[k];
        char *xs[] = { r->id, r->dir, r->name, r->feature };
        for (size_t j = 0; j < sizeof xs / sizeof xs[0]; ++j) rp_mem_free(a, xs[j]);
    }
    for (size_t k = 0; k < ir->copy_count; ++k) {
        rp_ir_copy_t *cp = &ir->copies[k];
        char *xs[] = { cp->id, cp->source_file, cp->dir, cp->name };
        for (size_t j = 0; j < sizeof xs / sizeof xs[0]; ++j) rp_mem_free(a, xs[j]);
    }
    for (size_t k = 0; k < ir->env_count; ++k) {
        rp_ir_env_t *e = &ir->envs[k];
        char *xs[] = { e->id, e->name, e->value, e->feature };
        for (size_t j = 0; j < sizeof xs / sizeof xs[0]; ++j) rp_mem_free(a, xs[j]);
    }
    rp_mem_free(a, ir->envs);
    for (size_t k = 0; k < ir->ini_count; ++k) {
        rp_ir_ini_t *x = &ir->inis[k];
        char *xs[] = { x->id, x->dir, x->file, x->section, x->key, x->value, x->feature };
        for (size_t j = 0; j < sizeof xs / sizeof xs[0]; ++j) rp_mem_free(a, xs[j]);
    }
    rp_mem_free(a, ir->inis);
    rp_mem_free(a, ir->removes);
    rp_mem_free(a, ir->copies);
    rp_mem_free(a, ir->properties);
    rp_mem_free(a, ir->actions);
    rp_mem_free(a, ir->arp_help);
    rp_mem_free(a, ir->arp_about);
    rp_mem_free(a, ir->folders);
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
    if (ir->refuse_below) {         // only when set: older goldens stay as they are
        kv(&b, "refuse-upgrade-below", ir->refuse_below);
        kv(&b, "refuse-upgrade-message", ir->refuse_message);
    }
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
                if (d->implicit) rp_buf_puts(&b, " implicit=1");
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
    // Folders in ID order.
    {
        size_t *order = rp_mem_alloc(alloc, ir->folder_count + 1, sizeof *order);
        for (size_t k = 0; order && k < ir->folder_count; ++k) order[k] = k;
        for (size_t i = 1; order && i < ir->folder_count; ++i) {
            for (size_t j = i; j > 0 && cmp_str(ir->folders[order[j - 1]].id, ir->folders[order[j]].id) > 0; --j) {
                size_t t = order[j];
                order[j] = order[j - 1];
                order[j - 1] = t;
            }
        }
        for (size_t k = 0; order && k < ir->folder_count; ++k) {
            const rp_ir_folder_t *f = &ir->folders[order[k]];
            rp_buf_puts(&b, "folder ");
            rp_buf_puts(&b, f->id);
            kv(&b, "dir", f->dir);
            kv(&b, "name", f->name);
            kv(&b, "keep", f->keep ? "1" : "0");
            kv(&b, "feature", f->feature);
            rp_buf_byte(&b, '\n');
        }
        rp_mem_free(alloc, order);
    }
    // RFC-0003 items, only when present (older goldens stay as they are).
    if (ir->arp_no_modify || ir->arp_no_repair || ir->arp_help || ir->arp_about) {
        rp_buf_puts(&b, "arp");
        kv(&b, "no-modify", ir->arp_no_modify ? "1" : "0");
        kv(&b, "no-repair", ir->arp_no_repair ? "1" : "0");
        kv(&b, "help", ir->arp_help);
        kv(&b, "about", ir->arp_about);
        rp_buf_byte(&b, '\n');
    }
    for (size_t k = 0; k < ir->property_count; ++k) {
        const rp_ir_property_t *p = &ir->properties[k];
        rp_buf_puts(&b, "property ");
        rp_buf_puts(&b, p->id);
        kv(&b, "value", p->value);
        kv(&b, "secure", p->secure ? "1" : "0");
        kv(&b, "hidden", p->hidden ? "1" : "0");
        rp_buf_byte(&b, '\n');
    }
    for (size_t k = 0; k < ir->action_count; ++k) {
        const rp_ir_action_t *a = &ir->actions[k];
        rp_buf_puts(&b, "action ");
        rp_buf_puts(&b, a->id);
        kv(&b, "run", a->run_file);
        kv(&b, "do", a->do_args);
        kv(&b, "undo", a->undo_args);
        kv(&b, "check", a->check_args);
        rp_buf_byte(&b, '\n');
    }
    static const char *const roots[] = { "HKCR", "HKCU", "HKLM" };
    static const char *const rtypes[] = { "string", "expand", "dword", "binary", "multi" };
    for (size_t k = 0; k < ir->registry_count; ++k) {
        const rp_ir_registry_t *r = &ir->registries[k];
        rp_buf_puts(&b, "registry ");
        rp_buf_puts(&b, r->id);
        kv(&b, "root", roots[r->root]);
        kv(&b, "key", r->key);
        kv(&b, "name", r->name);
        kv(&b, "type", rtypes[r->type]);
        kv(&b, "value", r->value);
        for (size_t j = 0; j < r->item_count; ++j) kv(&b, "item", r->items[j]);
        kv(&b, "remove", r->remove ? "1" : "0");
        kv(&b, "keep", r->keep ? "1" : "0");
        kv(&b, "view", r->view32 ? "32" : "native");
        kv(&b, "with", r->with_file);
        kv(&b, "feature", r->feature);
        rp_buf_byte(&b, '\n');
    }
    static const char *const modes[] = { "-", "install", "uninstall", "both" };
    for (size_t k = 0; k < ir->remove_count; ++k) {
        const rp_ir_remove_t *r = &ir->removes[k];
        rp_buf_puts(&b, "remove ");
        rp_buf_puts(&b, r->id);
        kv(&b, "dir", r->dir);
        kv(&b, "name", r->name);
        kv(&b, "on", modes[r->mode & 3]);
        kv(&b, "feature", r->feature);
        rp_buf_byte(&b, '\n');
    }
    static const char *const imodes[] = { "set", "add", "remove" };
    for (size_t k = 0; k < ir->ini_count; ++k) {
        const rp_ir_ini_t *x = &ir->inis[k];
        rp_buf_puts(&b, "ini ");
        rp_buf_puts(&b, x->id);
        kv(&b, "dir", x->dir);
        kv(&b, "file", x->file);
        kv(&b, "section", x->section);
        kv(&b, "key", x->key);
        kv(&b, "value", x->value);
        kv(&b, "mode", imodes[x->mode]);
        kv(&b, "feature", x->feature);
        rp_buf_byte(&b, '\n');
    }
    static const char *const emodes[] = { "set", "append", "prepend" };
    for (size_t k = 0; k < ir->env_count; ++k) {
        const rp_ir_env_t *e = &ir->envs[k];
        rp_buf_puts(&b, "env ");
        rp_buf_puts(&b, e->id);
        kv(&b, "name", e->name);
        kv(&b, "value", e->value);
        kv(&b, "mode", emodes[e->mode]);
        kv(&b, "keep", e->keep ? "1" : "0");
        kv(&b, "feature", e->feature);
        rp_buf_byte(&b, '\n');
    }
    for (size_t k = 0; k < ir->copy_count; ++k) {
        const rp_ir_copy_t *cp = &ir->copies[k];
        rp_buf_puts(&b, "copy ");
        rp_buf_puts(&b, cp->id);
        kv(&b, "source", cp->source_file);
        kv(&b, "dir", cp->dir);
        kv(&b, "name", cp->name);
        rp_buf_byte(&b, '\n');
    }
    for (size_t k = 0; k < ir->shortcut_count; ++k) {
        const rp_ir_shortcut_t *sc = &ir->shortcuts[k];
        rp_buf_puts(&b, "shortcut ");
        rp_buf_puts(&b, sc->id);
        kv(&b, "dir", sc->dir);
        kv(&b, "name", sc->name);
        kv(&b, "target", sc->target_file);
        kv(&b, "args", sc->args);
        kv(&b, "description", sc->description);
        kv(&b, "working-dir", sc->working_dir);
        rp_buf_byte(&b, '\n');
    }
    return rp_buf_take(&b, out, len);
}
