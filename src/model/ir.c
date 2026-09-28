// src/model/ir.c - `.rpk` AST -> checked package model (include/rubrapack/ir.h, RFC-0002).

#include "rubrapack/buf.h"
#include "rubrapack/ident.h"
#include "rubrapack/ir.h"
#include "rubrapack/mem.h"
#include "rubrapack/pal.h"
#include "rubrapack/pe.h"
#include "rubrapack/text.h"
#include "rubrapack/ui.h"

#include <stdio.h>
#include <stdlib.h>
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

// "text-ko" -> "ko" when `key` is `base` + "-" + a language code (2 or 3 lower-case letters), RFC-0012.
static const char *lang_of_key(const char *key, const char *base) {
    size_t n = strlen(base);
    if (strncmp(key, base, n) != 0 || key[n] != '-') return NULL;
    const char *code = key + n + 1;
    size_t m = strlen(code);
    if (m < 2 || m > 3) return NULL;
    for (size_t i = 0; i < m; ++i) {
        if (code[i] < 'a' || code[i] > 'z') return NULL;
    }
    return code;
}

// Like check_keys, and also takes `base-xx` for every base in `lang_bases` (NULL-terminated).
static void check_keys_lang(ctx_t *c, const rp_ttable_t *t, const char *const *allowed, const char *const *lang_bases) {
    for (size_t k = 0; k < t->count; ++k) {
        bool known = false;
        for (size_t j = 0; allowed[j]; ++j) known |= strcmp(t->keys[k].key, allowed[j]) == 0;
        for (size_t j = 0; lang_bases && lang_bases[j]; ++j) known |= lang_of_key(t->keys[k].key, lang_bases[j]) != NULL;
        // msi-only (RFC-0009 M6) is read for every item table outside [msix-*] (msix_blockers).
        known |= t->id && strncmp(t->kind, "msix", 4) != 0 && strcmp(t->keys[k].key, "msi-only") == 0;
        if (!known) {
            const char *hint = suggest(t->keys[k].key, allowed);
            ERR(c, t->keys[k].pos, "RP1201", "unknown key '%s' in [%s%s%s]%s%s%s", t->keys[k].key, t->kind, t->id ? "." : "",
                t->id ? t->id : "", hint ? " (did you mean '" : "", hint ? hint : "", hint ? "'?)" : "");
        }
    }
}

static void check_keys(ctx_t *c, const rp_ttable_t *t, const char *const *allowed) { check_keys_lang(c, t, allowed, NULL); }

static const rp_tkey_t *find_key(const rp_ttable_t *t, const char *key) {
    for (size_t k = 0; k < t->count; ++k) {
        if (strcmp(t->keys[k].key, key) == 0) return &t->keys[k];
    }
    return NULL;
}

static bool msix_output(const ctx_t *c) {
    const char *o = c->opt->output;
    size_t n = o ? strlen(o) : 0;
    return (n >= 5 && strcmp(o + n - 5, ".msix") == 0) || (n >= 11 && strcmp(o + n - 11, ".msixbundle") == 0);
}

// The built-in $(ARCH): the architecture being built - --arch, else [package] arch as written.
static const char *builtin_arch(ctx_t *c) {
    if (c->opt->arch) return c->opt->arch;
    for (size_t k = 0; k < c->doc->count; ++k) {
        const rp_ttable_t *t = &c->doc->tables[k];
        if (strcmp(t->kind, "package") != 0 || t->id) continue;
        const rp_tkey_t *a = find_key(t, "arch");
        if (a && a->val.kind == RP_TV_STRING && memchr(a->val.str, '$', a->val.len) == NULL) return a->val.str;
    }
    return NULL;
}

// $(NAME) substitution, once (RFC-0002 5): -D first, then [define], then the built-in $(ARCH);
// values are not re-read.
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
            if (!val && strcmp(name, "ARCH") == 0) val = builtin_arch(c);
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

// Every `base-xx` string of the table (RFC-0012), in source order.
static void get_ltexts(ctx_t *c, const rp_ttable_t *t, const char *base, rp_ir_ltext_t **out, size_t *count) {
    size_t n = 0;
    *out = NULL;
    *count = 0;
    for (size_t k = 0; k < t->count; ++k) n += lang_of_key(t->keys[k].key, base) != NULL;
    if (n == 0) return;
    *out = rp_mem_alloc(c->alloc, n, sizeof **out);
    if (*out == NULL) {
        c->nomem = true;
        return;
    }
    for (size_t k = 0; k < t->count; ++k) {
        const char *code = lang_of_key(t->keys[k].key, base);
        if (code == NULL) continue;
        if (t->keys[k].val.kind != RP_TV_STRING) {
            ERR(c, t->keys[k].pos, "RP1306", "'%s' must be a string", t->keys[k].key);
            continue;
        }
        (*out)[*count] = (rp_ir_ltext_t){ dup(c, code), subst(c, &t->keys[k].val) };
        ++*count;
    }
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

static const char *const top_kinds[] = { "package", "define", "arp", "ui", "msix", NULL };
static const char *const item_kinds[] = { "feature", "dir", "file", "files", "folder", "property", "action", "registry",
                                          "shortcut", "remove", "copy", "env", "ini", "require", "search", "service", "font", "permission",
                                          "ui-text", "dialog", "dialog-control", "assoc", "protocol", "msix-extension", NULL };
static const char *const later_kinds[] = { NULL };
static const char *const all_kinds[] = { "package", "define", "arp", "property", "feature", "dir", "file", "files", "folder",
                                         "registry", "shortcut", "env", "ini", "service", "assoc", "protocol",
                                         "font", "permission", "require", "search", "remove", "copy", "action",
                                         "arp", "ui", "ui-text", "dialog", "dialog-control", "msix", "msix-app", "msix-extension", NULL };

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
                                        "downgrade-message", "compress", "cab", "cab-max-size", "refuse-upgrade-below",
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
    } else if (home && arch && strcmp(home, arch) != 0 && !msix_output(c)) {   // an MSIX has no upgrade code
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
    if (scope && strcmp(scope, "user") == 0) ir->scope = 1;
    else if (scope && strcmp(scope, "dual") == 0) ir->scope = 2;
    else if (scope && strcmp(scope, "machine") != 0) {
        ERR(c, key_pos(t, "scope"), "RP1308", "scope must be \"machine\", \"user\" or \"dual\"");
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
    static const char *const sets[] = { "none", "basic", "minimal", "installdir", "features" };
    for (int k = 0; ui && k < 5; ++k) {
        if (strcmp(ui, sets[k]) == 0) {
            ir->ui = k;
            rp_mem_free(c->alloc, ui);
            ui = NULL;
        }
    }
    if (ui) ERR(c, key_pos(t, "ui"), "RP1316", "ui must be none, basic, minimal, installdir or features (got '%s')", ui);
    rp_mem_free(c->alloc, ui);
    ir->license_shown = get_str(c, t, "license", false, NULL);      // checked in ui_checks (RFC-0005 K3)
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
    if (cab && strcmp(cab, "external") == 0) {
        ir->cab_external = true;
    } else if (cab && strcmp(cab, "embed") != 0) {
        ERR(c, key_pos(t, "cab"), "RP1308", "cab must be \"embed\" or \"external\"");
    }
    rp_mem_free(c->alloc, cab);
    // Split: at most this many MiB of (uncompressed) files per cabinet; a larger file gets its own.
    ir->cab_max = (uint64_t)get_int(c, t, "cab-max-size", 0, 1, 2047) << 20;
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
    static const char *const keys[] = { "path", "feature", "guard", NULL };
    check_keys(c, t, keys);
    check_id(c, t, 72);
    d->id = dup(c, t->id);
    d->pos = t->pos;
    d->guard = get_bool(c, t, "guard", false);
    d->feature = get_str(c, t, "feature", false, NULL);
    char *path = get_str(c, t, "path", true, NULL);
    if (path == NULL) return;
    // base/part/part..., or a known folder alone (the folder itself, e.g. "Fonts")
    size_t count = 1;
    for (const char *p = path; *p; ++p) count += *p == '/';
    if (count == 1 && known_folder(path)) {
        d->base = path;
        d->parts = rp_mem_alloc(c->alloc, 1, sizeof *d->parts);     // non-NULL: a valid dir with no parts
        if (d->parts == NULL) c->nomem = true;
        return;
    }
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
    f->msi_only = get_bool(c, t, "msi-only", false);
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
    bool msi_only = get_bool(c, t, "msi-only", false);
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
                f->msi_only = msi_only;
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

// ---- [msix] and [msix-app.ID] (RFC-0009) --------------------------------------------------------

// ST_PackageName: 3-50 characters of A-Z a-z 0-9 . -
static bool msix_name_ok(const char *s) {
    size_t n = strlen(s);
    if (n < 3 || n > 50) return false;
    for (; *s; ++s) {
        char ch = *s;
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '.' || ch == '-')) return false;
    }
    return true;
}

static bool four_part_version(const char *s) {
    int parts = 0;
    while (*s) {
        unsigned v = 0, digits = 0;
        while (*s >= '0' && *s <= '9' && digits < 6) v = v * 10 + (unsigned)(*s++ - '0'), ++digits;
        if (digits == 0 || v > 65535) return false;
        ++parts;
        if (*s == '.') ++s;
        else if (*s) return false;
    }
    return parts == 4;
}

static void parse_msix(ctx_t *c, const rp_ttable_t *t) {
    static const char *const keys[] = { "identity-name", "publisher", "publisher-display-name", "min-version", NULL };
    check_keys(c, t, keys);
    rp_ir_t *ir = c->ir;
    ir->has_msix = true;
    ir->msix_pos = t->pos;
    ir->msix_identity_name = get_str(c, t, "identity-name", true, NULL);
    ir->msix_publisher = get_str(c, t, "publisher", true, NULL);
    ir->msix_publisher_display = get_str(c, t, "publisher-display-name", false, NULL);
    ir->msix_min_version = get_str(c, t, "min-version", false, NULL);
    if (ir->msix_identity_name && !msix_name_ok(ir->msix_identity_name)) {
        ERR(c, key_pos(t, "identity-name"), "RP1601", "identity-name must be 3 to 50 characters of A-Z, a-z, 0-9, '.' and '-' (got '%s')",
            ir->msix_identity_name);
    }
    if (ir->msix_publisher && (strchr(ir->msix_publisher, '=') == NULL || strlen(ir->msix_publisher) > 8192)) {
        ERR(c, key_pos(t, "publisher"), "RP1602", "publisher must be the signing certificate's subject, such as \"CN=Example, O=Example, C=KR\"");
    }
    if (ir->msix_publisher && strstr(ir->msix_publisher, "OID.2.25.311729368913984317654407730594956997722")) {
        ERR(c, key_pos(t, "publisher"), "RP1602", "publisher must not carry the unsigned-test OID; --unsigned-test adds it");
    }
    if (ir->msix_min_version && !four_part_version(ir->msix_min_version)) {
        ERR(c, key_pos(t, "min-version"), "RP1603", "min-version must have four parts, such as 10.0.17763.0");
    }
}

static char *logo_path(ctx_t *c, const rp_ttable_t *t, const char *key, char **shown) {
    *shown = get_str(c, t, key, false, NULL);
    if (*shown == NULL) return NULL;
    rp_pos_t p = key_pos(t, key);
    if (!source_path_ok(c, *shown, p)) return NULL;
    char *path = join(c, c->opt->source_dir ? c->opt->source_dir : ".", *shown);
    uint64_t size;
    if (path && rp_pal_stat(c->alloc, path, &size) != RP_FS_FILE) ERR(c, p, "RP1507", "logo '%s' not found", *shown);
    return path;
}

static void parse_msix_app(ctx_t *c, const rp_ttable_t *t) {
    static const char *const keys[] = { "executable", "display-name", "description", "logo-150", "logo-44", "store-logo", NULL };
    check_keys(c, t, keys);
    rp_ir_t *ir = c->ir;
    if (ir->msix_app_count == 100) {
        ERR(c, t->pos, "RP1606", "at most 100 [msix-app.*] tables");
        return;
    }
    rp_ir_msix_app_t *na = rp_mem_alloc(c->alloc, ir->msix_app_count + 1, sizeof *na);
    if (na == NULL) {
        c->nomem = true;
        return;
    }
    if (ir->msix_app_count) memcpy(na, ir->msix_apps, ir->msix_app_count * sizeof *na);
    rp_mem_free(c->alloc, ir->msix_apps);
    ir->msix_apps = na;
    rp_ir_msix_app_t *a = &na[ir->msix_app_count++];
    memset(a, 0, sizeof *a);
    a->id = dup(c, t->id);
    a->pos = t->pos;
    a->exe = get_str(c, t, "executable", true, NULL);
    a->display = get_str(c, t, "display-name", false, NULL);
    a->description = get_str(c, t, "description", false, NULL);
    static const char *const logos[3] = { "logo-150", "logo-44", "store-logo" };
    int given = 0;
    for (int i = 0; i < 3; ++i) {
        a->logo_path[i] = logo_path(c, t, logos[i], &a->logo[i]);
        given += a->logo[i] != NULL;
    }
    if (given != 0 && given != 3) ERR(c, t->pos, "RP1608", "give all three logos (logo-150, logo-44, store-logo) or none");
    bool id_ok = t->id[0] != '\0' && strlen(t->id) <= 64 && !(t->id[0] >= '0' && t->id[0] <= '9');
    for (const char *p = t->id; *p; ++p) id_ok &= (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9');
    if (!id_ok) ERR(c, t->pos, "RP1606", "the application ID '%s' becomes Application Id: letters and digits only, not starting with a digit", t->id);
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
    r->msi_only = get_bool(c, t, "msi-only", false);
    char *root = get_str(c, t, "root", true, NULL);
    if (root) {
        // Per scope: machine HKLM/HKCR/HKMU, user HKCU/HKCR/HKMU, dual HKMU/HKCR (HKMU = HKLM for a
        // per-machine installation, HKCU for a per-user one).
        int scope = c->ir->scope;
        if (strcmp(root, "HKLM") == 0) r->root = RP_ROOT_HKLM;
        else if (strcmp(root, "HKCR") == 0) r->root = RP_ROOT_HKCR;
        else if (strcmp(root, "HKCU") == 0) r->root = RP_ROOT_HKCU;
        else if (strcmp(root, "HKMU") == 0) r->root = RP_ROOT_HKMU;
        else ERR(c, key_pos(t, "root"), "RP1316", "root must be HKLM, HKCU, HKCR or HKMU (got '%s')", root);
        if (r->root == RP_ROOT_HKCU && scope == 0) {
            ERR(c, key_pos(t, "root"), "RP1316", "a per-machine package does not write HKCU (it would be the installing user's); use scope = \"user\"");
        } else if (r->root == RP_ROOT_HKLM && scope != 0) {
            ERR(c, key_pos(t, "root"), "RP1316", "a %s package cannot write HKLM; use HKMU", scope == 1 ? "per-user" : "dual");
        } else if (r->root == RP_ROOT_HKCU && scope == 2) {
            ERR(c, key_pos(t, "root"), "RP1316", "a dual package writes HKMU (HKLM or HKCU as installed), not HKCU");
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
    static const char *const types[] = { "string", "expand", "dword", "binary", "multi", "qword" };
    r->type = RP_REG_STRING;
    if (type) {
        bool known = false;
        for (int k = 0; k < 6; ++k) {
            if (strcmp(type, types[k]) == 0) r->type = (rp_reg_type_t)k, known = true;
        }
        if (!known) ERR(c, key_pos(t, "type"), "RP1316", "type must be string, expand, dword, qword, binary or multi (got '%s')", type);
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
    case RP_REG_QWORD:              // an integer, or "0x" and up to 16 hex digits for values past 2^63
        if (v->val.kind == RP_TV_INT && v->val.i >= 0) {
            snprintf(num, sizeof num, "%016llX", (unsigned long long)v->val.i);
        } else if (v->val.kind == RP_TV_STRING && v->val.str[0] == '0' && (v->val.str[1] | 32) == 'x' && v->val.len > 2 && v->val.len <= 18) {
            bool ok = true;
            for (size_t k = 2; k < v->val.len; ++k) ok &= is_hex(v->val.str[k]);
            if (!ok) {
                ERR(c, v->pos, "RP1316", "a qword value is an integer or \"0x\" and 1 to 16 hex digits");
                return;
            }
            unsigned long long q = strtoull(v->val.str + 2, NULL, 16);
            snprintf(num, sizeof num, "%016llX", q);
        } else {
            ERR(c, v->pos, "RP1316", "a qword value is an integer or \"0x\" and 1 to 16 hex digits");
            return;
        }
        r->value = dup(c, num);
        return;
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

// A basic shape check of an MSI condition (RFC-0001 9.7): quotes close, brackets balance.
static bool condition_ok(const char *s) {
    int depth = 0;
    bool quote = false;
    for (const char *p = s; *p; ++p) {
        if (*p == '"') quote = !quote;
        else if (!quote && *p == '(') ++depth;
        else if (!quote && *p == ')' && --depth < 0) return false;
    }
    return !quote && depth == 0 && s[0] != '\0';
}

static void parse_require(ctx_t *c, const rp_ttable_t *t, rp_ir_require_t *r) {
    static const char *const keys[] = { "condition", "message", NULL };
    check_keys(c, t, keys);
    check_id(c, t, 72);
    r->id = dup(c, t->id);
    r->pos = t->pos;
    r->condition = get_str(c, t, "condition", true, NULL);
    if (r->condition && !condition_ok(r->condition)) {
        ERR(c, key_pos(t, "condition"), "RP1316", "condition has an unclosed quote or unbalanced parentheses");
    }
    if (r->condition && strlen(r->condition) > 240) ERR(c, key_pos(t, "condition"), "RP1316", "condition is longer than 240 characters (rubrapack adds \"Installed OR ( )\")");
    r->message = get_str(c, t, "message", true, NULL);
}

static void parse_search(ctx_t *c, const rp_ttable_t *t, rp_ir_search_t *x) {
    static const char *const keys[] = { "property", "kind", "root", "key", "name", "view", "path", "file", "min-version",
                                        "component-guid", NULL };
    check_keys(c, t, keys);
    check_id(c, t, 72);
    x->id = dup(c, t->id);
    x->pos = t->pos;
    x->property = get_str(c, t, "property", true, NULL);
    if (x->property) {
        bool upper = true;
        for (const char *p = x->property; *p; ++p) upper &= (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_';
        if (!upper || tool_property(x->property)) {
            ERR(c, key_pos(t, "property"), "RP1310", "search property '%s' must be a public name of your own (upper case)", x->property);
        }
    }
    char *kind = get_str(c, t, "kind", true, NULL);
    if (kind == NULL) return;
    if (strcmp(kind, "registry") == 0) x->kind = RP_SEARCH_REGISTRY;
    else if (strcmp(kind, "file") == 0) x->kind = RP_SEARCH_FILE;
    else if (strcmp(kind, "dir") == 0) x->kind = RP_SEARCH_DIR;
    else if (strcmp(kind, "component") == 0) x->kind = RP_SEARCH_COMPONENT;
    else ERR(c, key_pos(t, "kind"), "RP1316", "kind must be registry, file, dir or component");
    rp_mem_free(c->alloc, kind);
    if (x->kind == RP_SEARCH_REGISTRY) {
        char *root = get_str(c, t, "root", true, NULL);
        if (root) {
            if (strcmp(root, "HKLM") == 0) x->root = RP_ROOT_HKLM;
            else if (strcmp(root, "HKCR") == 0) x->root = RP_ROOT_HKCR;
            else if (strcmp(root, "HKCU") == 0) x->root = RP_ROOT_HKCU;
            else ERR(c, key_pos(t, "root"), "RP1316", "root must be HKLM, HKCR or HKCU");
            rp_mem_free(c->alloc, root);
        }
        x->key = get_str(c, t, "key", true, NULL);
        x->name = get_str(c, t, "name", false, NULL);
        char *view = get_str(c, t, "view", false, NULL);
        if (view) {
            if (strcmp(view, "32") == 0) x->view32 = true;
            else if (strcmp(view, "64") != 0) ERR(c, key_pos(t, "view"), "RP1316", "view must be \"32\" or \"64\"");
            rp_mem_free(c->alloc, view);
        }
        if (c->ir->arch == RP_ARCH_X86) x->view32 = true;
    } else if (x->kind == RP_SEARCH_COMPONENT) {
        x->component_guid = get_str(c, t, "component-guid", true, NULL);
        if (x->component_guid && !guid_ok(x->component_guid)) ERR(c, key_pos(t, "component-guid"), "RP1308", "component-guid must be a GUID");
    } else {
        char *path = get_str(c, t, "path", true, NULL);    // Base or Base/rel/path, like [dir.*]
        if (path) {
            char *slash = strchr(path, '/');
            x->base = slash ? dup_n(c, path, (size_t)(slash - path)) : dup(c, path);
            if (x->base && !known_folder(x->base)) ERR(c, key_pos(t, "path"), "RP1316", "path must start with a known folder (like ProgramFiles or System)");
            if (slash && slash[1]) {
                x->path = dup(c, slash + 1);
                for (char *p = x->path; p && *p; ++p) {
                    if (*p == '/') *p = '\\';
                }
            }
            if (strchr(path, '\\')) ERR(c, key_pos(t, "path"), "RP1502", "use '/' in path");
            rp_mem_free(c->alloc, path);
        }
        if (x->kind == RP_SEARCH_FILE) {
            x->file_name = get_str(c, t, "file", true, NULL);
            if (x->file_name) target_name_ok(c, x->file_name, key_pos(t, "file"));
            x->min_version = get_str(c, t, "min-version", false, NULL);
            uint16_t parts[4];
            size_t n = 0;
            if (x->min_version && !parse_version(x->min_version, parts, &n)) {
                ERR(c, key_pos(t, "min-version"), "RP1308", "min-version must be a version like 1.2.3");
            }
        }
    }
}

static int ascii_casecmp(const char *a, const char *b) {
    for (;; ++a, ++b) {
        char x = (char)(*a >= 'A' && *a <= 'Z' ? *a + 32 : *a), y = (char)(*b >= 'A' && *b <= 'Z' ? *b + 32 : *b);
        if (x != y || x == 0) return x - y;
    }
}

static void parse_service(ctx_t *c, const rp_ttable_t *t, rp_ir_service_t *x) {
    static const char *const keys[] = { "file", "name", "display-name", "description", "start", "account", "args",
                                        "start-on-install", NULL };
    check_keys(c, t, keys);
    check_id(c, t, 72);
    x->id = dup(c, t->id);
    x->pos = t->pos;
    char *file = get_str(c, t, "file", true, NULL);
    if (file) {
        if (strncmp(file, "file:", 5) != 0 || file[5] == '\0') ERR(c, key_pos(t, "file"), "RP1315", "file must be \"file:<ID>\"");
        else x->file = dup(c, file + 5);
        rp_mem_free(c->alloc, file);
    }
    x->name = get_str(c, t, "name", true, NULL);
    if (x->name && (strpbrk(x->name, "/\\") || has_control(x->name) || strlen(x->name) > 256)) {
        ERR(c, key_pos(t, "name"), "RP1316", "a service name may not contain '/' or '\\' (at most 256 characters)");
    }
    x->display_name = get_str(c, t, "display-name", false, NULL);
    x->description = get_str(c, t, "description", false, NULL);
    x->args = get_str(c, t, "args", false, NULL);
    x->start = 3;
    char *start = get_str(c, t, "start", false, NULL);
    if (start) {
        if (strcmp(start, "auto") == 0) x->start = 2;
        else if (strcmp(start, "demand") == 0) x->start = 3;
        else if (strcmp(start, "disabled") == 0) x->start = 4;
        else ERR(c, key_pos(t, "start"), "RP1316", "start must be auto, demand or disabled");
        rp_mem_free(c->alloc, start);
    }
    char *account = get_str(c, t, "account", false, NULL);
    if (account) {
        if (strcmp(account, "LocalSystem") == 0) x->account = 0;
        else if (strcmp(account, "LocalService") == 0) x->account = 1;
        else if (strcmp(account, "NetworkService") == 0) x->account = 2;
        else ERR(c, key_pos(t, "account"), "RP1316", "account must be LocalSystem, LocalService or NetworkService");
        rp_mem_free(c->alloc, account);
    }
    x->start_on_install = get_bool(c, t, "start-on-install", false);
    if (x->start_on_install && x->start == 4) ERR(c, key_pos(t, "start-on-install"), "RP1316", "a disabled service cannot be started");
}

static void parse_permission(ctx_t *c, const rp_ttable_t *t, rp_ir_permission_t *x) {
    static const char *const keys[] = { "target", "sddl", NULL };
    check_keys(c, t, keys);
    check_id(c, t, 72);
    x->id = dup(c, t->id);
    x->pos = t->pos;
    char *target = get_str(c, t, "target", true, NULL);
    if (target) {
        static const char *const prefixes[] = { "dir:", "file:", "registry:" };
        x->kind = -1;
        for (int k = 0; k < 3; ++k) {
            size_t n = strlen(prefixes[k]);
            if (strncmp(target, prefixes[k], n) == 0 && target[n]) {
                x->kind = k;
                x->target = dup(c, target + n);
            }
        }
        if (x->kind < 0) ERR(c, key_pos(t, "target"), "RP1315", "target must be \"dir:<ID>\", \"file:<ID>\" or \"registry:<ID>\"");
        rp_mem_free(c->alloc, target);
    }
    x->sddl = get_str(c, t, "sddl", true, NULL);
    if (x->sddl) {
        bool ok = strlen(x->sddl) >= 3 && strchr("DOGS", x->sddl[0]) && x->sddl[1] == ':';
        for (const char *p = x->sddl; ok && *p; ++p) ok = *p > ' ' && *p < 127 && *p != '[' && *p != ']';
        if (!ok) ERR(c, key_pos(t, "sddl"), "RP1316", "sddl must be an SDDL string like \"D:PAI(A;OICI;FA;;;BA)\"");
    }
}

static void parse_font(ctx_t *c, const rp_ttable_t *t, rp_ir_font_t *x) {
    static const char *const keys[] = { "file", "title", NULL };
    check_keys(c, t, keys);
    check_id(c, t, 72);
    x->id = dup(c, t->id);
    x->pos = t->pos;
    char *file = get_str(c, t, "file", true, NULL);
    if (file) {
        if (strncmp(file, "file:", 5) != 0 || file[5] == '\0') ERR(c, key_pos(t, "file"), "RP1315", "file must be \"file:<ID>\"");
        else x->file = dup(c, file + 5);
        rp_mem_free(c->alloc, file);
    }
    x->title = get_str(c, t, "title", false, NULL);
}

// "file:<ID>" -> the ID (a copy), or NULL with RP1315.
static char *file_ref(ctx_t *c, const rp_ttable_t *t, const char *key, bool required) {
    char *v = get_str(c, t, key, required, NULL);
    if (v == NULL) return NULL;
    char *id = NULL;
    if (strncmp(v, "file:", 5) != 0 || v[5] == '\0') ERR(c, key_pos(t, key), "RP1315", "%s must be \"file:<ID>\" naming a [file.*] of this package", key);
    else id = dup(c, v + 5);
    rp_mem_free(c->alloc, v);
    return id;
}

// Lower-case ASCII letters, digits and `extra`, starting at `from`; at least `min` of them.
static bool lower_name(const char *s, const char *extra, size_t min) {
    size_t n = 0;
    for (; *s; ++s, ++n) {
        if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || strchr(extra, *s))) return false;
    }
    return n >= min;
}

static void parse_assoc(ctx_t *c, const rp_ttable_t *t, rp_ir_assoc_t *x) {
    static const char *const keys[] = { "extension", "prog-id", "description", "target", "icon", "args", "msi-only", NULL };
    check_keys(c, t, keys);
    check_id(c, t, 64);
    x->id = dup(c, t->id);
    x->pos = t->pos;
    x->extension = get_str(c, t, "extension", true, NULL);
    if (x->extension && (x->extension[0] != '.' || strlen(x->extension) > 64 || !lower_name(x->extension + 1, "_-", 1))) {
        ERR(c, key_pos(t, "extension"), "RP1316", "extension must be '.' and lower-case letters, digits, '_' or '-' (got '%s')", x->extension);
    }
    x->prog_id = get_str(c, t, "prog-id", true, NULL);
    if (x->prog_id) {
        bool ok = strlen(x->prog_id) <= 39 && x->prog_id[0] && !(x->prog_id[0] >= '0' && x->prog_id[0] <= '9');
        for (const char *q = x->prog_id; ok && *q; ++q) {
            ok = (*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') || (*q >= '0' && *q <= '9') || *q == '.' || *q == '_' || *q == '-';
        }
        if (!ok) ERR(c, key_pos(t, "prog-id"), "RP1316", "prog-id must be letters, digits, '.', '_' or '-', not starting with a digit, at most 39 (got '%s')", x->prog_id);
    }
    x->description = get_str(c, t, "description", false, NULL);
    x->target_file = file_ref(c, t, "target", true);
    x->icon_file = file_ref(c, t, "icon", false);
    x->args = get_str(c, t, "args", false, NULL);
    if (x->args == NULL) x->args = dup(c, "\"%1\"");
}

static void parse_protocol(ctx_t *c, const rp_ttable_t *t, rp_ir_protocol_t *x) {
    static const char *const keys[] = { "name", "description", "target", "args", "msi-only", NULL };
    check_keys(c, t, keys);
    check_id(c, t, 64);
    x->id = dup(c, t->id);
    x->pos = t->pos;
    x->name = get_str(c, t, "name", true, NULL);
    if (x->name && (!(x->name[0] >= 'a' && x->name[0] <= 'z') || strlen(x->name) > 64 || !lower_name(x->name, "+-.", 2))) {
        ERR(c, key_pos(t, "name"), "RP1316", "a scheme is a lower-case letter, then lower-case letters, digits, '+', '-' or '.' (got '%s')", x->name);
    }
    static const char *const taken[] = { "http", "https", "file", "ftp", "mailto", "ms-settings", "shell", NULL };
    if (x->name && in_list(x->name, taken)) ERR(c, key_pos(t, "name"), "RP1316", "'%s' belongs to Windows or the browsers", x->name);
    x->description = get_str(c, t, "description", false, NULL);
    x->target_file = file_ref(c, t, "target", true);
    x->args = get_str(c, t, "args", false, NULL);
    if (x->args == NULL) x->args = dup(c, "\"%1\"");
}

static void parse_msix_ext(ctx_t *c, const rp_ttable_t *t, rp_ir_msix_ext_t *x) {
    static const char *const keys[] = { "kind", "app", "alias", "task-id", "display-name", "enabled", NULL };
    check_keys(c, t, keys);
    check_id(c, t, 72);
    x->id = dup(c, t->id);
    x->pos = t->pos;
    char *kind = get_str(c, t, "kind", true, NULL);
    x->app = get_str(c, t, "app", false, NULL);
    x->enabled = get_bool(c, t, "enabled", true);
    if (kind && strcmp(kind, "alias") == 0) {
        x->kind = RP_MSIX_EXT_ALIAS;
        x->alias = get_str(c, t, "alias", true, NULL);
        size_t n = x->alias ? strlen(x->alias) : 0;
        bool ok = n > 4 && n <= 64 && strcmp(x->alias + n - 4, ".exe") == 0;
        for (size_t k = 0; ok && k < n; ++k) {
            char ch = x->alias[k];
            ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-';
        }
        if (x->alias && !ok) ERR(c, key_pos(t, "alias"), "RP1316", "alias must be a file name ending in .exe: letters, digits, '.', '_', '-' (got '%s')", x->alias);
        if (find_key(t, "task-id") || find_key(t, "display-name") || find_key(t, "enabled")) {
            ERR(c, t->pos, "RP1316", "task-id, display-name and enabled belong to kind = \"startup-task\"");
        }
    } else if (kind && strcmp(kind, "startup-task") == 0) {
        x->kind = RP_MSIX_EXT_STARTUP;
        x->task_id = get_str(c, t, "task-id", false, NULL);
        if (x->task_id == NULL) x->task_id = dup(c, t->id);
        x->display = get_str(c, t, "display-name", false, NULL);
        if (find_key(t, "alias")) ERR(c, key_pos(t, "alias"), "RP1316", "alias belongs to kind = \"alias\"");
    } else if (kind) {
        ERR(c, key_pos(t, "kind"), "RP1316", "kind must be \"alias\" or \"startup-task\" (got '%s')", kind);
    }
    rp_mem_free(c->alloc, kind);
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

// ---- dialogs (RFC-0005) ------------------------------------------------------------------------

static bool source_path_ok(ctx_t *c, const char *s, rp_pos_t pos);
static char *join(ctx_t *c, const char *a, const char *b);

static bool ends_with_ci(const char *s, const char *suffix) {
    size_t n = strlen(s), m = strlen(suffix);
    if (n < m) return false;
    for (size_t k = 0; k < m; ++k) {
        char a = s[n - m + k];
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (a != suffix[k]) return false;
    }
    return true;
}

// A source file named by a key (license, banner): relative, existing, a regular file.
static char *ui_source(ctx_t *c, const char *shown, rp_pos_t pos) {
    if (!source_path_ok(c, shown, pos)) return NULL;
    char *path = join(c, c->opt->source_dir, shown);
    uint64_t size = 0;
    if (path && rp_pal_stat(c->alloc, path, &size) != RP_FS_FILE) {
        ERR(c, pos, "RP1509", "cannot read '%s'", shown);
        rp_mem_free(c->alloc, path);
        return NULL;
    }
    return path;
}

static void parse_ui_text(ctx_t *c, const rp_ttable_t *t, rp_ir_ui_text_t *x) {
    static const char *const keys[] = { "text", NULL }, *const lkeys[] = { "text", NULL };
    check_keys_lang(c, t, keys, lkeys);
    x->id = dup(c, t->id);
    x->pos = t->pos;
    get_ltexts(c, t, "text", &x->by_lang, &x->by_lang_count);
    x->text = get_str(c, t, "text", x->by_lang_count == 0, NULL);
    if (!rp_ui_text_known(t->id)) ERR(c, t->pos, "RP1201", "[ui-text.%s]: no dialog text has this ID", t->id);
}

static bool public_property(const char *s) {
    bool upper = s[0] != '\0' && strlen(s) <= 72 && !(s[0] >= '0' && s[0] <= '9');
    for (const char *p = s; *p; ++p) upper &= (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_';
    return upper && !tool_property(s);
}

// Names the built-in frame gives every page (rubrapack/ui.h); an author's control cannot take them.
static bool frame_control(const char *s) {
    static const char *const names[] = { "Banner", "Title", "Description", "BannerLine", "BottomLine", "Back", "Next",
                                         "Cancel", NULL };
    return in_list(s, names);
}

static void parse_dialog(ctx_t *c, const rp_ttable_t *t, rp_ir_dialog_t *x) {
    static const char *const keys[] = { "title", "description", "after", NULL }, *const lkeys[] = { "title", "description", NULL };
    check_keys_lang(c, t, keys, lkeys);
    get_ltexts(c, t, "title", &x->title_by_lang, &x->title_by_lang_count);
    get_ltexts(c, t, "description", &x->description_by_lang, &x->description_by_lang_count);
    x->id = dup(c, t->id);
    x->pos = t->pos;
    if (!check_id(c, t, 72)) return;
    if (strncmp(t->id, "Rp", 2) == 0 || strcmp(t->id, "FilesInUse") == 0) {
        ERR(c, t->pos, "RP1302", "dialog IDs starting with 'Rp' (and FilesInUse) are rubrapack's own");
    }
    x->title = get_str(c, t, "title", false, NULL);         // the banner heading; [ProductName] when omitted
    x->description = get_str(c, t, "description", false, NULL);
    x->after = get_str(c, t, "after", true, NULL);
}

static void parse_dialog_control(ctx_t *c, const rp_ttable_t *t, rp_ir_dialog_control_t *x) {
    static const char *const keys[] = { "dialog", "type", "x", "y", "width", "height", "text", "property", "values",
                                        "labels", NULL }, *const lkeys[] = { "text", "labels", NULL };
    check_keys_lang(c, t, keys, lkeys);
    get_ltexts(c, t, "text", &x->text_by_lang, &x->text_by_lang_count);
    x->id = dup(c, t->id);
    x->pos = t->pos;
    if (!check_id(c, t, 50)) return;
    if (frame_control(t->id)) ERR(c, t->pos, "RP1302", "'%s' is a control of the built-in frame; choose another ID", t->id);
    x->dialog = get_str(c, t, "dialog", true, NULL);
    char *type = get_str(c, t, "type", true, NULL);
    static const char *const types[] = { "text", "checkbox", "edit", "radio", "combo", NULL };
    x->type = -1;
    for (int k = 0; type && types[k]; ++k) {
        if (strcmp(type, types[k]) == 0) x->type = k;
    }
    if (type && x->type < 0) ERR(c, key_pos(t, "type"), "RP1316", "type must be text, checkbox, edit, radio or combo (got '%s')", type);
    rp_mem_free(c->alloc, type);
    static const char *const req[] = { "x", "y", "width", "height" };
    for (int k = 0; k < 4; ++k) {
        if (!find_key(t, req[k])) ERR(c, t->pos, "RP1202", "[dialog-control.%s] needs '%s'", t->id, req[k]);
    }
    // The body between the banner line (44) and the button line (234) of a 370 x 270 page.
    x->x = (int)get_int(c, t, "x", 0, 0, 369);
    x->y = (int)get_int(c, t, "y", 45, 45, 233);
    x->width = (int)get_int(c, t, "width", 1, 1, 370);
    x->height = (int)get_int(c, t, "height", 1, 1, 189);
    if (x->x + x->width > 370) ERR(c, key_pos(t, "width"), "RP1308", "x + width must be at most 370 (the page width)");
    if (x->y + x->height > 234) ERR(c, key_pos(t, "height"), "RP1308", "y + height must be at most 234 (the line above the buttons)");
    bool has_text = false;
    x->text = get_str(c, t, "text", false, &has_text);
    x->property = get_str(c, t, "property", false, NULL);
    if (x->type == RP_DC_TEXT || x->type == RP_DC_CHECKBOX) {
        if (!has_text && x->text_by_lang_count == 0) ERR(c, t->pos, "RP1202", "[dialog-control.%s] needs 'text'", t->id);
    } else if (x->type >= 0 && (has_text || x->text_by_lang_count)) {
        ERR(c, key_pos(t, "text"), "RP1316", "a %s control has no text; put a text control beside it", x->type == RP_DC_EDIT ? "edit" : x->type == RP_DC_RADIO ? "radio" : "combo");
    }
    if (x->type == RP_DC_TEXT) {
        if (x->property) ERR(c, key_pos(t, "property"), "RP1316", "a text control has no property");
    } else if (x->type >= 0) {
        if (x->property == NULL) ERR(c, t->pos, "RP1202", "[dialog-control.%s] needs 'property'", t->id);
        else if (!public_property(x->property))
            ERR(c, key_pos(t, "property"), "RP1310", "property '%s' must be a public name of your own (upper case)", x->property);
    }
    const rp_tkey_t *vk = find_key(t, "values"), *lk = find_key(t, "labels");
    bool list = x->type == RP_DC_RADIO || x->type == RP_DC_COMBO;
    if (!list) {
        if (vk) ERR(c, vk->pos, "RP1316", "only radio and combo controls take values");
        if (lk) ERR(c, lk->pos, "RP1316", "only radio and combo controls take labels");
        for (size_t k = 0; k < t->count; ++k) {
            if (lang_of_key(t->keys[k].key, "labels")) ERR(c, t->keys[k].pos, "RP1316", "only radio and combo controls take labels");
        }
        return;
    }
    if (vk == NULL) {
        ERR(c, t->pos, "RP1202", "[dialog-control.%s] needs 'values'", t->id);
        return;
    }
    if (vk->val.kind != RP_TV_ARRAY || vk->val.count == 0 || vk->val.count > 32) {
        ERR(c, vk->pos, "RP1316", "values is an array of 1 to 32 strings");
        return;
    }
    if (lk && (lk->val.kind != RP_TV_ARRAY || lk->val.count != vk->val.count)) {
        ERR(c, lk->pos, "RP1316", "labels is an array of strings, one per value");
        lk = NULL;
    }
    x->values = rp_mem_alloc(c->alloc, vk->val.count, sizeof *x->values);
    x->labels = rp_mem_alloc(c->alloc, vk->val.count, sizeof *x->labels);
    if (x->values == NULL || x->labels == NULL) {
        c->nomem = true;
        return;
    }
    for (size_t k = 0; k < vk->val.count; ++k) {
        const rp_tval_t *v = &vk->val.items[k], *l = lk ? &lk->val.items[k] : NULL;
        if (v->kind != RP_TV_STRING || (l && l->kind != RP_TV_STRING)) {
            ERR(c, vk->pos, "RP1316", "values and labels are strings");
            break;
        }
        char *val = subst(c, v);
        if (val && (val[0] == '\0' || strlen(val) > 64)) ERR(c, vk->pos, "RP1316", "a value is 1 to 64 bytes");
        for (size_t j = 0; val && j < x->value_count; ++j) {
            if (strcmp(x->values[j], val) == 0) ERR(c, vk->pos, "RP1316", "value '%s' is listed twice", val);
        }
        x->values[x->value_count] = val;
        x->labels[x->value_count] = l ? subst(c, l) : dup(c, val ? val : "");
        ++x->value_count;
    }
    // labels-xx: the labels in one language, one per value (RFC-0012).
    size_t nl = 0;
    for (size_t k = 0; k < t->count; ++k) nl += lang_of_key(t->keys[k].key, "labels") != NULL;
    if (nl) {
        x->labels_by_lang = rp_mem_alloc(c->alloc, nl, sizeof *x->labels_by_lang);
        if (x->labels_by_lang == NULL) {
            c->nomem = true;
            return;
        }
    }
    for (size_t k = 0; k < t->count; ++k) {
        const char *code = lang_of_key(t->keys[k].key, "labels");
        if (code == NULL) continue;
        const rp_tval_t *a = &t->keys[k].val;
        if (a->kind != RP_TV_ARRAY || a->count != x->value_count) {
            ERR(c, t->keys[k].pos, "RP1316", "%s is an array of strings, one per value", t->keys[k].key);
            continue;
        }
        char **ls = rp_mem_alloc(c->alloc, a->count, sizeof *ls);
        if (ls == NULL) {
            c->nomem = true;
            return;
        }
        for (size_t j = 0; j < a->count; ++j) {
            if (a->items[j].kind != RP_TV_STRING) {
                ERR(c, t->keys[k].pos, "RP1316", "%s holds strings", t->keys[k].key);
                ls[j] = dup(c, "");
            } else {
                ls[j] = subst(c, &a->items[j]);
            }
        }
        x->labels_by_lang[x->labels_by_lang_count++] = (rp_ir_llabels_t){ dup(c, code), ls };
    }
    if (x->type == RP_DC_RADIO && x->height < 12 * (int)x->value_count)
        ERR(c, key_pos(t, "height"), "RP1308", "a radio control stacks its buttons: height must be at least 12 per value (%d)", 12 * (int)x->value_count);
}

static const rp_ir_dialog_t *find_dialog(const rp_ir_t *ir, const char *id) {
    for (size_t k = 0; k < ir->dialog_count; ++k) {
        if (ir->dialogs[k].id && strcmp(ir->dialogs[k].id, id) == 0) return &ir->dialogs[k];
    }
    return NULL;
}

static const rp_ir_property_t *find_property(const rp_ir_t *ir, const char *id) {
    for (size_t k = 0; k < ir->property_count; ++k) {
        if (ir->properties[k].id && strcmp(ir->properties[k].id, id) == 0) return &ir->properties[k];
    }
    return NULL;
}

// K4: every page hangs off a page of the chosen set; the values have defaults (RFC-0003 7).
static void dialog_checks(ctx_t *c) {
    rp_ir_t *ir = c->ir;
    for (size_t k = 0; k < ir->dialog_count; ++k) {
        const rp_ir_dialog_t *d = &ir->dialogs[k];
        if (ir->ui < RP_UI_MINIMAL) {
            ERR(c, d->pos, "RP1316", "[dialog.%s] needs ui = \"minimal\", \"installdir\" or \"features\"", d->id);
            continue;
        }
        if (d->after == NULL) continue;
        bool builtin = strcmp(d->after, "RpWelcomeDlg") == 0 ||
                       (strcmp(d->after, "RpLicenseDlg") == 0 && ir->license_shown) ||
                       (strcmp(d->after, "RpInstallDirDlg") == 0 && ir->ui >= RP_UI_INSTALLDIR) ||
                       (strcmp(d->after, "RpCustomizeDlg") == 0 && ir->ui == RP_UI_FEATURES);
        if (builtin) continue;
        // Another author page: it must exist and the chain must reach a built-in page.
        const rp_ir_dialog_t *p = find_dialog(ir, d->after);
        size_t steps = 0;
        while (p && p->after && find_dialog(ir, p->after) && steps <= ir->dialog_count) {
            p = find_dialog(ir, p->after);
            ++steps;
        }
        if (find_dialog(ir, d->after) == NULL) {
            ERR(c, d->pos, "RP1315", "after = '%s' is not a page of this dialog set (RpWelcomeDlg%s%s%s) or a [dialog.*]", d->after,
                ir->license_shown ? ", RpLicenseDlg" : "", ir->ui >= RP_UI_INSTALLDIR ? ", RpInstallDirDlg" : "",
                ir->ui == RP_UI_FEATURES ? ", RpCustomizeDlg" : "");
        } else if (steps > ir->dialog_count) {
            ERR(c, d->pos, "RP1307", "[dialog.%s] is placed after itself through other dialogs", d->id);
        }
    }
    for (size_t k = 0; k < ir->dialog_control_count; ++k) {
        const rp_ir_dialog_control_t *x = &ir->dialog_controls[k];
        if (x->dialog && find_dialog(ir, x->dialog) == NULL) ERR(c, x->pos, "RP1315", "dialog '%s' is not a [dialog.*]", x->dialog);
        if (x->property == NULL || x->type == RP_DC_TEXT) continue;
        const rp_ir_property_t *p = find_property(ir, x->property);
        if (x->type == RP_DC_RADIO || x->type == RP_DC_COMBO) {
            bool ok = false;
            for (size_t j = 0; p && p->value && j < x->value_count; ++j) ok |= x->values[j] && strcmp(x->values[j], p->value) == 0;
            if (!ok) ERR(c, x->pos, "RP1315", "a %s control needs [property.%s] with one of its values as the default (a silent installation uses it)",
                         x->type == RP_DC_RADIO ? "radio" : "combo", x->property);
        }
        for (size_t j = 0; j < k; ++j) {
            const rp_ir_dialog_control_t *y = &ir->dialog_controls[j];
            if (y->property && strcmp(y->property, x->property) == 0 && y->type != RP_DC_TEXT)
                ERR(c, x->pos, "RP1301", "property '%s' already belongs to control '%s'", x->property, y->id);
        }
    }
    for (size_t k = 0; k < ir->dialog_count; ++k) {
        bool any = false;
        for (size_t j = 0; j < ir->dialog_control_count && !any; ++j) any = ir->dialog_controls[j].dialog && strcmp(ir->dialog_controls[j].dialog, ir->dialogs[k].id) == 0;
        size_t count = 0;
        for (size_t j = 0; j < ir->dialog_control_count; ++j) count += ir->dialog_controls[j].dialog && strcmp(ir->dialog_controls[j].dialog, ir->dialogs[k].id) == 0;
        if (!any) ERR(c, ir->dialogs[k].pos, "RP1202", "[dialog.%s] has no [dialog-control.*]", ir->dialogs[k].id);
        else if (count > 64) ERR(c, ir->dialogs[k].pos, "RP1313", "[dialog.%s] has %zu controls; at most 64", ir->dialogs[k].id, count);
    }
}

// --nfc (RFC-0006 L3): the names the package gives to folders, files and shortcuts in NFC - a
// file from macOS often arrives decomposed. The source files keep their names; texts, registry
// values and the rest stay as written (lint warns about them). The case check that follows sees
// the new names, so two names that become one are refused.
static void nfc_one(ctx_t *c, char **s) {
    if (*s == NULL) return;
    uint8_t *o;
    size_t n;
    proven_err_t err = rp_nfc(c->alloc, (const uint8_t *)*s, strlen(*s), &o, &n);
    if (err == PROVEN_ERR_NOMEM) {
        c->nomem = true;
        return;
    }
    if (err != PROVEN_OK) return;       // not UTF-8: already refused where the name was read
    rp_mem_free(c->alloc, *s);
    *s = (char *)o;
}

static void nfc_names(ctx_t *c) {
    rp_ir_t *ir = c->ir;
    for (size_t k = 0; k < ir->dir_count; ++k) {
        for (size_t j = 0; j < ir->dirs[k].part_count; ++j) nfc_one(c, &ir->dirs[k].parts[j]);
    }
    for (size_t k = 0; k < ir->file_count; ++k) nfc_one(c, &ir->files[k].name);
    for (size_t k = 0; k < ir->folder_count; ++k) nfc_one(c, &ir->folders[k].name);
    for (size_t k = 0; k < ir->shortcut_count; ++k) nfc_one(c, &ir->shortcuts[k].name);
    for (size_t k = 0; k < ir->copy_count; ++k) nfc_one(c, &ir->copies[k].name);
}

// Languages the dialogs know without help (RFC-0012): their LANGIDs for the automatic choice, the
// name on the language page and the face. Built-in texts exist for en and ko only; any other
// language gives every text itself ([ui-text.ID] text-xx).
static const struct {
    const char *code, *name, *font;
    uint16_t    ids[6];
} known_langs[] = {
    { "en", "English", "Segoe UI", { 1033, 2057, 3081, 4105, 5129, 6153 } },
    { "ko", "한국어", "맑은 고딕", { 1042 } },
    { "ja", "日本語", "Yu Gothic UI", { 1041 } },
    { "zh", "中文", "Microsoft YaHei UI", { 2052, 1028, 3076, 4100, 5124 } },
    { "de", "Deutsch", "Segoe UI", { 1031, 2055, 3079, 4103, 5127 } },
    { "fr", "Français", "Segoe UI", { 1036, 2060, 3084, 4108, 5132, 6156 } },
    { "es", "Español", "Segoe UI", { 1034, 3082, 2058, 11274, 9226, 13322 } },
    { "it", "Italiano", "Segoe UI", { 1040, 2064 } },
    { "pt", "Português", "Segoe UI", { 1046, 2070 } },
    { "nl", "Nederlands", "Segoe UI", { 1043, 2067 } },
    { "pl", "Polski", "Segoe UI", { 1045 } },
    { "ru", "Русский", "Segoe UI", { 1049 } },
    { "uk", "Українська", "Segoe UI", { 1058 } },
    { "tr", "Türkçe", "Segoe UI", { 1055 } },
    { "vi", "Tiếng Việt", "Segoe UI", { 1066 } },
    { "th", "ไทย", "Leelawadee UI", { 1054 } },
};

static int known_lang(const char *code) {
    for (size_t i = 0; i < sizeof known_langs / sizeof known_langs[0]; ++i) {
        if (strcmp(known_langs[i].code, code) == 0) return (int)i;
    }
    return -1;
}

static int ui_lang_index(const rp_ir_t *ir, const char *code) {
    for (size_t i = 0; i < ir->ui_lang_count; ++i) {
        if (strcmp(ir->ui_langs[i].code, code) == 0) return (int)i;
    }
    return -1;
}

// A license file of the dialogs: .txt/.md shown as text, .rtf as it is.
static bool license_kind_ok(const char *s) { return ends_with_ci(s, ".txt") || ends_with_ci(s, ".rtf") || ends_with_ci(s, ".md"); }

// [ui] languages and its name-xx / font-xx / langid-xx / license-xx (RFC-0012). English is always
// there, first: the dialogs' default.
static void ui_languages(ctx_t *c, const rp_ttable_t *uit) {
    rp_ir_t *ir = c->ir;
    ir->ui_lang_count = 1;
    memcpy(ir->ui_langs[0].code, "en", 3);
    const rp_tkey_t *lk = uit ? find_key(uit, "languages") : NULL;
    if (lk) {
        if (lk->val.kind != RP_TV_ARRAY) ERR(c, lk->pos, "RP1306", "languages is an array of language codes, like [\"ko\"]");
        for (size_t k = 0; lk->val.kind == RP_TV_ARRAY && k < lk->val.count; ++k) {
            const rp_tval_t *v = &lk->val.items[k];
            char *code = v->kind == RP_TV_STRING ? subst(c, v) : NULL;
            size_t n = code ? strlen(code) : 0;
            bool ok = n >= 2 && n <= 3;
            for (size_t i = 0; ok && i < n; ++i) ok = code[i] >= 'a' && code[i] <= 'z';
            if (!ok) {
                ERR(c, lk->pos, "RP1316", "languages holds codes of 2 or 3 lower-case letters, like \"ko\" (got '%s')", code ? code : "?");
            } else if (strcmp(code, "en") == 0) {
                // English is always there
            } else if (ui_lang_index(ir, code) >= 0) {
                ERR(c, lk->pos, "RP1301", "language '%s' is listed twice", code);
            } else if (ir->ui_lang_count == RP_UI_LANG_MAX) {
                ERR(c, lk->pos, "RP1308", "at most %d languages besides English", RP_UI_LANG_MAX - 1);
            } else {
                memcpy(ir->ui_langs[ir->ui_lang_count++].code, code, n + 1);
            }
            rp_mem_free(c->alloc, code);
        }
        if (ir->ui_lang_count > 1 && ir->ui == 0) ERR(c, lk->pos, "RP1316", "languages needs a dialog set (ui = \"basic\" or another)");
    }
    // The per-language keys of [ui].
    for (size_t k = 0; uit && k < uit->count; ++k) {
        const rp_tkey_t *key = &uit->keys[k];
        static const char *const bases[] = { "name", "font", "langid", "license" };
        for (int b = 0; b < 4; ++b) {
            const char *code = lang_of_key(key->key, bases[b]);
            if (code == NULL) continue;
            int li = ui_lang_index(ir, code);
            if (li < 0) {
                ERR(c, key->pos, "RP1316", "'%s': '%s' is not in [ui] languages", key->key, code);
                continue;
            }
            rp_ir_ui_lang_t *L = &ir->ui_langs[li];
            L->pos = key->pos;
            if (b == 2) {           // langid-xx: one LANGID or an array of them
                const rp_tval_t *one = &key->val;
                size_t cnt = one->kind == RP_TV_ARRAY ? one->count : 1;
                for (size_t j = 0; j < cnt; ++j) {
                    const rp_tval_t *v = one->kind == RP_TV_ARRAY ? &one->items[j] : one;
                    if (v->kind != RP_TV_INT || v->i < 1 || v->i > 65535) {
                        ERR(c, key->pos, "RP1308", "%s holds LANGIDs (1..65535), like 1041", key->key);
                        break;
                    }
                    if (L->langid_count < RP_UI_LANGID_MAX) L->langids[L->langid_count++] = (uint16_t)v->i;
                }
                continue;
            }
            if (key->val.kind != RP_TV_STRING) {
                ERR(c, key->pos, "RP1306", "'%s' must be a string", key->key);
                continue;
            }
            char *v = subst(c, &key->val);
            if (b == 0) L->name = v;
            else if (b == 1) L->font = v;
            else {
                if (ir->ui < 2) ERR(c, key->pos, "RP1316", "a license needs ui = \"minimal\", \"installdir\" or \"features\"");
                else if (!license_kind_ok(v)) ERR(c, key->pos, "RP1316", "license must be a .txt, .md (shown as plain text) or .rtf file");
                else L->license_source = ui_source(c, v, key->pos);
                L->license_shown = v;
            }
        }
    }
    // What each language still needs.
    for (size_t i = 0; i < ir->ui_lang_count; ++i) {
        rp_ir_ui_lang_t *L = &ir->ui_langs[i];
        int kl = known_lang(L->code);
        rp_pos_t pos = lk ? lk->pos : (uit ? uit->pos : (rp_pos_t){ 0 });
        if (L->langid_count == 0 && kl >= 0) {
            for (size_t j = 0; j < 6 && known_langs[kl].ids[j]; ++j) L->langids[L->langid_count++] = known_langs[kl].ids[j];
        }
        if (L->langid_count == 0) ERR(c, pos, "RP1202", "language '%s' needs [ui] langid-%s (its LANGIDs, for the automatic choice)", L->code, L->code);
        if (L->name == NULL && kl < 0) ERR(c, pos, "RP1202", "language '%s' needs [ui] name-%s (its name on the language page)", L->code, L->code);
        if (L->name == NULL && kl >= 0) L->name = dup(c, known_langs[kl].name);
        if (L->font == NULL) L->font = dup(c, kl >= 0 ? known_langs[kl].font : "Segoe UI");
        // Built-in texts are English and Korean; another language writes every one of them.
        if (strcmp(L->code, "en") != 0 && strcmp(L->code, "ko") != 0) {
            size_t missing = 0;
            const char *first = NULL;
            for (size_t t = 0; rp_ui_text_id(t); ++t) {
                const char *id = rp_ui_text_id(t);
                bool have = false;
                for (size_t u = 0; u < ir->ui_text_count && !have; ++u) {
                    const rp_ir_ui_text_t *x = &ir->ui_texts[u];
                    if (strcmp(x->id, id) != 0) continue;
                    have = x->text != NULL;
                    for (size_t w = 0; w < x->by_lang_count && !have; ++w) have = strcmp(x->by_lang[w].lang, L->code) == 0;
                }
                if (!have && missing++ == 0) first = id;
            }
            if (missing) ERR(c, pos, "RP1202", "language '%s' has no built-in texts: give [ui-text.ID] text-%s for all of them (%zu missing, the first is %s)",
                             L->code, L->code, missing, first);
        }
    }
    // text-xx / title-xx / labels-xx name languages of the dialogs.
#define LANG_OK(code, pos)                                                                                             \
    do {                                                                                                               \
        if (ui_lang_index(ir, (code)) < 0) ERR(c, (pos), "RP1316", "language '%s' is not in [ui] languages", (code));  \
    } while (0)
    for (size_t u = 0; u < ir->ui_text_count; ++u) {
        for (size_t w = 0; w < ir->ui_texts[u].by_lang_count; ++w) LANG_OK(ir->ui_texts[u].by_lang[w].lang, ir->ui_texts[u].pos);
    }
    for (size_t u = 0; u < ir->dialog_count; ++u) {
        for (size_t w = 0; w < ir->dialogs[u].title_by_lang_count; ++w) LANG_OK(ir->dialogs[u].title_by_lang[w].lang, ir->dialogs[u].pos);
        for (size_t w = 0; w < ir->dialogs[u].description_by_lang_count; ++w) LANG_OK(ir->dialogs[u].description_by_lang[w].lang, ir->dialogs[u].pos);
    }
    for (size_t u = 0; u < ir->dialog_control_count; ++u) {
        const rp_ir_dialog_control_t *x = &ir->dialog_controls[u];
        for (size_t w = 0; w < x->text_by_lang_count; ++w) LANG_OK(x->text_by_lang[w].lang, x->pos);
        for (size_t w = 0; w < x->labels_by_lang_count; ++w) LANG_OK(x->labels_by_lang[w].lang, x->pos);
    }
#undef LANG_OK
    // 0.1 made Korean dialogs from language = "ko-KR"; the dialogs are English unless Korean is added.
    if (ir->language == 1042 && ir->ui && ui_lang_index(ir, "ko") < 0) {
        rp_srcdiag_add(c->d, uit ? uit->pos : (rp_pos_t){ 0 }, "RP1317", true,
                       "language = \"ko-KR\" no longer makes the dialogs Korean: they are English unless [ui] languages = [\"ko\"] adds Korean");
    }
}

static void ui_checks(ctx_t *c, const rp_ttable_t *uit, const rp_ttable_t *pkg) {
    rp_ir_t *ir = c->ir;
    if (ir->license_shown) {
        rp_pos_t pos = key_pos(pkg, "license");
        if (ir->ui == 0 || ir->ui == 1) ERR(c, pos, "RP1316", "a license needs ui = \"minimal\", \"installdir\" or \"features\"");
        else if (!ends_with_ci(ir->license_shown, ".txt") && !ends_with_ci(ir->license_shown, ".rtf") && !ends_with_ci(ir->license_shown, ".md")) {
            ERR(c, pos, "RP1316", "license must be a .txt, .md (shown as plain text) or .rtf file");
        } else {
            ir->license_source = ui_source(c, ir->license_shown, pos);
        }
    }
    ui_languages(c, uit);
    if (uit == NULL) return;
    static const char *const keys[] = { "banner", "install-dir", "languages", NULL },
                             *const lkeys[] = { "name", "font", "langid", "license", NULL };
    check_keys_lang(c, uit, keys, lkeys);
    if (ir->ui == 0) ERR(c, uit->pos, "RP1316", "[ui] needs ui = \"basic\" or another dialog set in [package]");
    char *banner = get_str(c, uit, "banner", false, NULL);
    if (banner) {
        if (!ends_with_ci(banner, ".bmp")) ERR(c, key_pos(uit, "banner"), "RP1316", "banner must be a .bmp file (Windows Installer shows BMP only)");
        else ir->banner_source = ui_source(c, banner, key_pos(uit, "banner"));
        rp_mem_free(c->alloc, banner);
    }
    ir->ui_install_dir = get_str(c, uit, "install-dir", false, NULL);
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

static int cmp_str(const char *a, const char *b) { return strcmp(a ? a : "", b ? b : ""); }

static const rp_ir_file_t *file_by_id(const rp_ir_t *ir, const char *id) {
    for (size_t j = 0; id && j < ir->file_count; ++j) {
        if (strcmp(ir->files[j].id, id) == 0) return &ir->files[j];
    }
    return NULL;
}

// A program of this package that opens files or URIs: a [file.*] whose name ends in .exe.
static bool program_ok(ctx_t *c, const char *id, rp_pos_t pos, const char *what) {
    if (id == NULL) return false;
    const rp_ir_file_t *f = file_by_id(c->ir, id);
    if (f == NULL) {
        ERR(c, pos, "RP1315", "%s: file '%s' is not a [file.*] of this package", what, id);
        return false;
    }
    const char *n = f->name ? f->name : "";
    size_t l = strlen(n);
    if (l < 4 || (strcmp(n + l - 4, ".exe") != 0 && strcmp(n + l - 4, ".EXE") != 0)) {
        ERR(c, pos, "RP1316", "%s: file '%s' is not a program (.exe)", what, id);
        return false;
    }
    return true;
}

static void add_class_value(ctx_t *c, const char *id, const char *suffix, const char *key, const char *name, const char *value,
                            const char *with, rp_pos_t pos) {
    rp_ir_registry_t *r = &c->ir->registries[c->ir->registry_count++];
    memset(r, 0, sizeof *r);
    char rid[96];
    snprintf(rid, sizeof rid, "%s.%s", id, suffix);
    r->id = dup(c, rid);
    r->root = RP_ROOT_HKCR;
    r->key = dup(c, key);
    r->name = name ? dup(c, name) : NULL;
    r->type = RP_REG_STRING;
    r->value = dup(c, value);
    r->msi_only = true;             // an MSIX says the same in its manifest (RFC-0010 N4)
    r->with_file = dup(c, with);
    r->pos = pos;
}

// [assoc.*], [protocol.*] and [msix-extension.*] (RFC-0010 N4): programs that exist, one handler per
// extension and scheme, one description per prog-id; then, for the MSI, their HKCR values in the
// program's component. HKCR follows the installation: HKLM\Software\Classes per machine,
// HKCU\Software\Classes per user.
static void class_checks(ctx_t *c) {
    rp_ir_t *ir = c->ir;
    for (size_t k = 0; k < ir->assoc_count; ++k) {
        rp_ir_assoc_t *x = &ir->assocs[k];
        program_ok(c, x->target_file, x->pos, "target");
        if (x->icon_file && file_by_id(ir, x->icon_file) == NULL) ERR(c, x->pos, "RP1315", "icon: file '%s' is not a [file.*] of this package", x->icon_file);
        for (size_t j = 0; j < k; ++j) {
            const rp_ir_assoc_t *y = &ir->assocs[j];
            if (x->extension && y->extension && strcmp(x->extension, y->extension) == 0) {
                ERR(c, x->pos, "RP1301", "extension '%s' already has a program ([assoc.%s])", x->extension, y->id);
            }
            if (x->prog_id && y->prog_id && strcmp(x->prog_id, y->prog_id) == 0 &&
                (cmp_str(x->description, y->description) != 0 || cmp_str(x->target_file, y->target_file) != 0 ||
                 cmp_str(x->icon_file, y->icon_file) != 0 || cmp_str(x->args, y->args) != 0)) {
                ERR(c, x->pos, "RP1316", "prog-id '%s' is also in [assoc.%s] with another description, target, icon or args", x->prog_id, y->id);
            }
        }
    }
    for (size_t k = 0; k < ir->protocol_count; ++k) {
        rp_ir_protocol_t *x = &ir->protocols[k];
        program_ok(c, x->target_file, x->pos, "target");
        for (size_t j = 0; j < k; ++j) {
            if (x->name && ir->protocols[j].name && strcmp(x->name, ir->protocols[j].name) == 0) {
                ERR(c, x->pos, "RP1301", "scheme '%s' already has a program ([protocol.%s])", x->name, ir->protocols[j].id);
            }
        }
    }
    for (size_t k = 0; k < ir->msix_ext_count; ++k) {
        rp_ir_msix_ext_t *x = &ir->msix_exts[k];
        bool found = x->app == NULL;
        for (size_t j = 0; !found && j < ir->msix_app_count; ++j) found = strcmp(ir->msix_apps[j].id, x->app) == 0;
        if (!found) ERR(c, x->pos, "RP1315", "app '%s' is not an [msix-app.*]", x->app);
        for (size_t j = 0; j < k; ++j) {
            const rp_ir_msix_ext_t *y = &ir->msix_exts[j];
            if (x->kind == RP_MSIX_EXT_ALIAS && y->kind == RP_MSIX_EXT_ALIAS && cmp_str(x->app, y->app) == 0) {
                ERR(c, x->pos, "RP1301", "an application has one alias ([msix-extension.%s] is one already)", y->id);
            }
            if (x->kind == RP_MSIX_EXT_STARTUP && y->kind == RP_MSIX_EXT_STARTUP && strcmp(x->task_id, y->task_id) == 0) {
                ERR(c, x->pos, "RP1301", "task-id '%s' is already used by [msix-extension.%s]", x->task_id, y->id);
            }
        }
    }
    if (c->d->errors) return;       // a table with a missing key has NULL fields; the build fails anyway
    for (size_t k = 0; k < ir->assoc_count && !c->nomem; ++k) {
        const rp_ir_assoc_t *x = &ir->assocs[k];
        add_class_value(c, x->id, "Ext", x->extension, NULL, x->prog_id, x->target_file, x->pos);
        bool first = true;              // a prog-id shared by several extensions is written once
        for (size_t j = 0; j < k; ++j) first &= strcmp(ir->assocs[j].prog_id, x->prog_id) != 0;
        if (!first) continue;
        char key[160], val[512];
        add_class_value(c, x->id, "Prog", x->prog_id, NULL, x->description ? x->description : ir->name, x->target_file, x->pos);
        snprintf(key, sizeof key, "%s\\DefaultIcon", x->prog_id);
        snprintf(val, sizeof val, "[#%s],0", x->icon_file ? x->icon_file : x->target_file);
        add_class_value(c, x->id, "Icon", key, NULL, val, x->target_file, x->pos);
        snprintf(key, sizeof key, "%s\\shell\\open\\command", x->prog_id);
        char *cmd = rp_mem_alloc(c->alloc, strlen(x->target_file) + strlen(x->args) + 16, 1);
        if (cmd == NULL) {
            c->nomem = true;
            return;
        }
        sprintf(cmd, "\"[#%s]\" %s", x->target_file, x->args);
        add_class_value(c, x->id, "Cmd", key, NULL, cmd, x->target_file, x->pos);
        rp_mem_free(c->alloc, cmd);
    }
    for (size_t k = 0; k < ir->protocol_count && !c->nomem; ++k) {
        const rp_ir_protocol_t *x = &ir->protocols[k];
        char key[160], val[512];
        snprintf(val, sizeof val, "URL:%s", x->description ? x->description : x->name);
        add_class_value(c, x->id, "Url", x->name, NULL, val, x->target_file, x->pos);
        add_class_value(c, x->id, "Proto", x->name, "URL Protocol", "", x->target_file, x->pos);
        snprintf(key, sizeof key, "%s\\DefaultIcon", x->name);
        snprintf(val, sizeof val, "[#%s],0", x->target_file);
        add_class_value(c, x->id, "Icon", key, NULL, val, x->target_file, x->pos);
        snprintf(key, sizeof key, "%s\\shell\\open\\command", x->name);
        char *cmd = rp_mem_alloc(c->alloc, strlen(x->target_file) + strlen(x->args) + 16, 1);
        if (cmd == NULL) {
            c->nomem = true;
            return;
        }
        sprintf(cmd, "\"[#%s]\" %s", x->target_file, x->args);
        add_class_value(c, x->id, "Cmd", key, NULL, cmd, x->target_file, x->pos);
        rp_mem_free(c->alloc, cmd);
    }
}

static void cross_checks(ctx_t *c) {
    rp_ir_t *ir = c->ir;
    // IDs unique across dir, file and feature (RFC-0002 2).
    typedef struct { const char *id; rp_pos_t pos; } idpos_t;
    size_t n = ir->dir_count + ir->file_count + ir->feature_count + ir->folder_count + ir->registry_count +
               ir->shortcut_count + ir->remove_count + ir->copy_count + ir->env_count + ir->ini_count +
               ir->require_count + ir->search_count + ir->service_count + ir->font_count + ir->permission_count +
               ir->dialog_count + ir->dialog_control_count + ir->assoc_count + ir->protocol_count + ir->msix_ext_count;
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
    for (size_t k = 0; k < ir->require_count; ++k) ids[m++] = (idpos_t){ ir->requires[k].id, ir->requires[k].pos };
    for (size_t k = 0; k < ir->search_count; ++k) ids[m++] = (idpos_t){ ir->searches[k].id, ir->searches[k].pos };
    for (size_t k = 0; k < ir->service_count; ++k) ids[m++] = (idpos_t){ ir->services[k].id, ir->services[k].pos };
    for (size_t k = 0; k < ir->font_count; ++k) ids[m++] = (idpos_t){ ir->fonts[k].id, ir->fonts[k].pos };
    for (size_t k = 0; k < ir->assoc_count; ++k) ids[m++] = (idpos_t){ ir->assocs[k].id, ir->assocs[k].pos };
    for (size_t k = 0; k < ir->protocol_count; ++k) ids[m++] = (idpos_t){ ir->protocols[k].id, ir->protocols[k].pos };
    for (size_t k = 0; k < ir->msix_ext_count; ++k) ids[m++] = (idpos_t){ ir->msix_exts[k].id, ir->msix_exts[k].pos };
    for (size_t k = 0; k < ir->permission_count; ++k) ids[m++] = (idpos_t){ ir->permissions[k].id, ir->permissions[k].pos };
    for (size_t k = 0; k < ir->dialog_count; ++k) ids[m++] = (idpos_t){ ir->dialogs[k].id, ir->dialogs[k].pos };
    for (size_t k = 0; k < ir->dialog_control_count; ++k) ids[m++] = (idpos_t){ ir->dialog_controls[k].id, ir->dialog_controls[k].pos };
    for (size_t k = 0; k < ir->feature_count; ++k) {
        if (!ir->features[k].implicit) ids[m++] = (idpos_t){ ir->features[k].id, ir->features[k].pos };
    }
    for (size_t a = 0; a < m; ++a) {
        for (size_t b = a + 1; b < m; ++b) {
            if (ids[a].id && ids[b].id && strcmp(ids[a].id, ids[b].id) == 0) {
                ERR(c, ids[b].pos, "RP1301", "ID '%s' is already used (line %u); IDs must differ across all tables",
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
    // Scope (RFC-0004 H3): what needs a per-machine installation.
    if (ir->scope != 0) {
        static const char *const scopes[] = { "machine", "user", "dual" };
        const char *sc = scopes[ir->scope];
        for (size_t k = 0; k < ir->service_count; ++k) ERR(c, ir->services[k].pos, "RP1316", "services need scope = \"machine\" (this is %s)", sc);
        for (size_t k = 0; k < ir->font_count; ++k) ERR(c, ir->fonts[k].pos, "RP1316", "fonts need scope = \"machine\" (this is %s)", sc);
        for (size_t k = 0; k < ir->permission_count; ++k) ERR(c, ir->permissions[k].pos, "RP1316", "permissions need scope = \"machine\" (this is %s)", sc);
        if (ir->scope == 2) {
            for (size_t k = 0; k < ir->env_count; ++k) ERR(c, ir->envs[k].pos, "RP1316", "a dual package cannot choose between a user and a system variable");
        }
        for (size_t k = 0; k < ir->dir_count; ++k) {
            const char *b = ir->dirs[k].base;
            if (b && (strcmp(b, "Windows") == 0 || strcmp(b, "System") == 0 || strcmp(b, "Fonts") == 0 || strcmp(b, "CommonAppData") == 0)) {
                ERR(c, ir->dirs[k].pos, "RP1316", "'%s' is a machine folder; a %s package cannot write there", b, sc);
            }
        }
    }
    // Dialogs: the folder the user may change is a dir of this package (default: INSTALLDIR).
    if (ir->ui >= 3) {
        const char *d = ir->ui_install_dir ? ir->ui_install_dir : "INSTALLDIR";
        if (!find_dir(ir, d)) {
            rp_pos_t top = { 1, 1 };
            ERR(c, top, "RP1315", "ui = \"%s\" lets the user choose the folder of dir '%s', which does not exist (set [ui] install-dir)",
                ir->ui == 3 ? "installdir" : "features", d);
        }
    }
    if (ir->ui == 4) {
        bool any = false;
        for (size_t k = 0; k < ir->feature_count; ++k) any |= !ir->features[k].implicit;
        if (!any) {
            rp_pos_t top = { 1, 1 };
            ERR(c, top, "RP1316", "ui = \"features\" needs [feature.*] tables to choose from");
        }
    }
    // Permissions: the target exists, once per target; a registry target must write a value.
    for (size_t k = 0; k < ir->permission_count; ++k) {
        rp_ir_permission_t *x = &ir->permissions[k];
        if (x->target == NULL) continue;
        bool found = false;
        if (x->kind == 0) {
            const rp_ir_dir_t *d = find_dir(ir, x->target);
            found = d != NULL;
            if (d && d->part_count == 0) ERR(c, x->pos, "RP1316", "a known folder itself ('%s') cannot get permissions", x->target);
            if (d) x->feature = dup(c, d->feature ? d->feature : declared ? NULL : "Main");
            if (d && x->feature == NULL) ERR(c, x->pos, "RP1202", "[permission.%s] needs its dir to have a feature", x->id);
        } else if (x->kind == 1) {
            for (size_t j = 0; j < ir->file_count; ++j) found |= strcmp(ir->files[j].id, x->target) == 0;
        } else {
            for (size_t j = 0; j < ir->registry_count; ++j) {
                if (strcmp(ir->registries[j].id, x->target) == 0) {
                    found = true;
                    if (ir->registries[j].remove) ERR(c, x->pos, "RP1316", "registry '%s' removes a value; it has nothing to protect", x->target);
                }
            }
        }
        if (!found) ERR(c, x->pos, "RP1315", "target '%s' does not exist", x->target);
        for (size_t j = 0; j < k; ++j) {
            if (ir->permissions[j].kind == x->kind && ir->permissions[j].target && strcmp(ir->permissions[j].target, x->target) == 0) {
                ERR(c, x->pos, "RP1301", "'%s' already has permissions", x->target);
            }
        }
    }
    // Fonts: a file installed directly in the Fonts folder ([dir.X] path = "Fonts"), once.
    for (size_t k = 0; k < ir->font_count; ++k) {
        const rp_ir_font_t *x = &ir->fonts[k];
        const rp_ir_file_t *f = NULL;
        for (size_t j = 0; x->file && j < ir->file_count; ++j) {
            if (strcmp(ir->files[j].id, x->file) == 0) f = &ir->files[j];
        }
        const rp_ir_dir_t *d = f && f->dir ? find_dir(ir, f->dir) : NULL;
        if (x->file && f == NULL) {
            ERR(c, x->pos, "RP1315", "file '%s' is not a [file.*] of this package", x->file);
        } else if (f && !(d && d->part_count == 0 && d->base && strcmp(d->base, "Fonts") == 0)) {
            ERR(c, x->pos, "RP1316", "file '%s' must be installed in the Fonts folder itself (a [dir.*] with path = \"Fonts\")", x->file);
        }
        for (size_t j = 0; j < k; ++j) {
            if (x->file && ir->fonts[j].file && strcmp(ir->fonts[j].file, x->file) == 0) {
                ERR(c, x->pos, "RP1301", "file '%s' is already registered as a font", x->file);
            }
        }
    }
    // Services: an exe of this package that its machines can run; one service per file and name.
    for (size_t k = 0; k < ir->service_count; ++k) {
        const rp_ir_service_t *x = &ir->services[k];
        const rp_ir_file_t *f = NULL;
        for (size_t j = 0; x->file && j < ir->file_count; ++j) {
            if (strcmp(ir->files[j].id, x->file) == 0) f = &ir->files[j];
        }
        if (x->file && f == NULL) {
            ERR(c, x->pos, "RP1315", "file '%s' is not a [file.*] of this package", x->file);
        } else if (f && (f->pe_machine == 0 || f->pe_is_dll)) {
            ERR(c, x->pos, "RP1315", "'%s' is not a program (.exe)", f->source);
        }
        for (size_t j = 0; j < k; ++j) {
            if (x->file && ir->services[j].file && strcmp(ir->services[j].file, x->file) == 0) {
                ERR(c, x->pos, "RP1316", "file '%s' already runs service '%s'", x->file, ir->services[j].id);
            }
            if (x->name && ir->services[j].name && ascii_casecmp(ir->services[j].name, x->name) == 0) {
                ERR(c, x->pos, "RP1301", "service name '%s' is already used", x->name);
            }
        }
    }
    // Launch conditions are keyed by their text; search properties are unique and not a
    // [property.*] of the package.
    for (size_t k = 0; k < ir->require_count; ++k) {
        for (size_t j = 0; j < k; ++j) {
            if (ir->requires[k].condition && ir->requires[j].condition && strcmp(ir->requires[k].condition, ir->requires[j].condition) == 0) {
                ERR(c, ir->requires[k].pos, "RP1301", "the same condition is already required (line %u)", (unsigned)ir->requires[j].pos.line);
            }
        }
    }
    for (size_t k = 0; k < ir->search_count; ++k) {
        const rp_ir_search_t *x = &ir->searches[k];
        if (x->property == NULL) continue;
        for (size_t j = 0; j < k; ++j) {
            if (ir->searches[j].property && strcmp(ir->searches[j].property, x->property) == 0) {
                ERR(c, x->pos, "RP1301", "property '%s' is already searched (line %u)", x->property, (unsigned)ir->searches[j].pos.line);
            }
        }
        for (size_t j = 0; j < ir->property_count; ++j) {
            if (strcmp(ir->properties[j].id, x->property) == 0) ERR(c, x->pos, "RP1310", "'%s' is also a [property.*]", x->property);
        }
        // A search may give a dir its default (RFC-0012 V5): a folder from the registry or from a
        // folder search, never a file's path.
        if (find_dir(ir, x->property)) {
            if (x->kind != RP_SEARCH_REGISTRY && x->kind != RP_SEARCH_DIR) {
                ERR(c, x->pos, "RP1310", "'%s' is the name of a dir: only a registry or dir search can fill a dir", x->property);
            } else {
                ir->searches[k].fills_dir = true;
            }
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
                ERR(c, where[b], "RP1511", "installs to the same path as line %u (Windows ignores case in names%s)",
                    (unsigned)where[a].line, c->opt->nfc ? "; --nfc made their Unicode forms the same" : "");
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
    size_t nfeat = 0, ndir = 0, nfile = 0, nfolder = 0, nprop = 0, naction = 0, nreg = 0, nshort = 0, nrem = 0, ncopy = 0, nenv = 0, nini = 0, nreq = 0, nsearch = 0, nsvc = 0, nfont = 0, nperm = 0, nuitext = 0, ndlg = 0, ndctl = 0, nassoc = 0, nproto = 0, next_ = 0;
    const rp_ttable_t *uit = NULL;
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
        else if (strcmp(t->kind, "require") == 0) ++nreq;
        else if (strcmp(t->kind, "search") == 0) ++nsearch;
        else if (strcmp(t->kind, "service") == 0) ++nsvc;
        else if (strcmp(t->kind, "font") == 0) ++nfont;
        else if (strcmp(t->kind, "assoc") == 0) ++nassoc;
        else if (strcmp(t->kind, "protocol") == 0) ++nproto;
        else if (strcmp(t->kind, "msix-extension") == 0) ++next_;
        else if (strcmp(t->kind, "permission") == 0) ++nperm;
        else if (strcmp(t->kind, "ui-text") == 0) ++nuitext;
        else if (strcmp(t->kind, "dialog") == 0) ++ndlg;
        else if (strcmp(t->kind, "dialog-control") == 0) ++ndctl;
        else if (strcmp(t->kind, "ui") == 0) uit = t;
        else if (strcmp(t->kind, "arp") == 0) arp = t;
        else if (strcmp(t->kind, "msix") == 0) parse_msix(&c, t);
        else if (strcmp(t->kind, "msix-app") == 0) parse_msix_app(&c, t);
        // RFC-0009 M6: what an MSIX cannot carry (yet) is an error there, unless msi-only = true.
        static const char *const msix_ok[] = { "package", "define", "dir", "file", "files", "feature", "property", "ui", "ui-text",
                                               "dialog", "dialog-control", "arp", "msix", "msix-app", "msix-extension", "registry",
                                               "assoc", "protocol", "shortcut", "font", NULL };
        if (!in_list(t->kind, msix_ok) && !get_bool(&c, t, "msi-only", false)) {
            rp_ir_msix_block_t *nb = rp_mem_alloc(alloc, ir->msix_block_count + 1, sizeof *nb);
            if (nb == NULL) {
                c.nomem = true;
            } else {
                if (ir->msix_block_count) memcpy(nb, ir->msix_blocks, ir->msix_block_count * sizeof *nb);
                rp_mem_free(alloc, ir->msix_blocks);
                ir->msix_blocks = nb;
                nb[ir->msix_block_count++] = (rp_ir_msix_block_t){ dup(&c, t->kind), dup(&c, t->id), t->pos };
            }
        }
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
    // [assoc] and [protocol] add their HKCR values for the MSI (add_class_values).
    ir->registries = rp_mem_alloc(alloc, nreg + 4 * nassoc + 4 * nproto + 1, sizeof *ir->registries);
    ir->assocs = rp_mem_alloc(alloc, nassoc + 1, sizeof *ir->assocs);
    ir->protocols = rp_mem_alloc(alloc, nproto + 1, sizeof *ir->protocols);
    ir->msix_exts = rp_mem_alloc(alloc, next_ + 1, sizeof *ir->msix_exts);
    if (ir->assocs == NULL || ir->protocols == NULL || ir->msix_exts == NULL) c.nomem = true;
    ir->shortcuts = rp_mem_alloc(alloc, nshort + 1, sizeof *ir->shortcuts);
    ir->removes = rp_mem_alloc(alloc, nrem + 1, sizeof *ir->removes);
    ir->copies = rp_mem_alloc(alloc, ncopy + 1, sizeof *ir->copies);
    ir->envs = rp_mem_alloc(alloc, nenv + 1, sizeof *ir->envs);
    ir->inis = rp_mem_alloc(alloc, nini + 1, sizeof *ir->inis);
    ir->requires = rp_mem_alloc(alloc, nreq + 1, sizeof *ir->requires);
    ir->searches = rp_mem_alloc(alloc, nsearch + 1, sizeof *ir->searches);
    ir->services = rp_mem_alloc(alloc, nsvc + 1, sizeof *ir->services);
    ir->fonts = rp_mem_alloc(alloc, nfont + 1, sizeof *ir->fonts);
    ir->permissions = rp_mem_alloc(alloc, nperm + 1, sizeof *ir->permissions);
    ir->ui_texts = rp_mem_alloc(alloc, nuitext + 1, sizeof *ir->ui_texts);
    ir->dialogs = rp_mem_alloc(alloc, ndlg + 1, sizeof *ir->dialogs);
    ir->dialog_controls = rp_mem_alloc(alloc, ndctl + 1, sizeof *ir->dialog_controls);
    if (ir->ui_texts == NULL || ir->dialogs == NULL || ir->dialog_controls == NULL) c.nomem = true;
    if (ir->services == NULL || ir->fonts == NULL || ir->permissions == NULL) c.nomem = true;
    if (ir->properties == NULL || ir->actions == NULL || ir->registries == NULL || ir->shortcuts == NULL ||
        ir->removes == NULL || ir->copies == NULL || ir->envs == NULL || ir->inis == NULL || ir->requires == NULL ||
        ir->searches == NULL) {
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
        } else if (strcmp(t->kind, "require") == 0) {
            rp_ir_require_t *r = &ir->requires[ir->require_count++];
            memset(r, 0, sizeof *r);
            parse_require(&c, t, r);
        } else if (strcmp(t->kind, "search") == 0) {
            rp_ir_search_t *x = &ir->searches[ir->search_count++];
            memset(x, 0, sizeof *x);
            parse_search(&c, t, x);
        } else if (strcmp(t->kind, "service") == 0) {
            rp_ir_service_t *x = &ir->services[ir->service_count++];
            memset(x, 0, sizeof *x);
            parse_service(&c, t, x);
        } else if (strcmp(t->kind, "font") == 0) {
            rp_ir_font_t *x = &ir->fonts[ir->font_count++];
            memset(x, 0, sizeof *x);
            parse_font(&c, t, x);
        } else if (strcmp(t->kind, "assoc") == 0) {
            rp_ir_assoc_t *x = &ir->assocs[ir->assoc_count++];
            memset(x, 0, sizeof *x);
            parse_assoc(&c, t, x);
        } else if (strcmp(t->kind, "protocol") == 0) {
            rp_ir_protocol_t *x = &ir->protocols[ir->protocol_count++];
            memset(x, 0, sizeof *x);
            parse_protocol(&c, t, x);
        } else if (strcmp(t->kind, "msix-extension") == 0) {
            rp_ir_msix_ext_t *x = &ir->msix_exts[ir->msix_ext_count++];
            memset(x, 0, sizeof *x);
            parse_msix_ext(&c, t, x);
        } else if (strcmp(t->kind, "permission") == 0) {
            rp_ir_permission_t *x = &ir->permissions[ir->permission_count++];
            memset(x, 0, sizeof *x);
            parse_permission(&c, t, x);
        } else if (strcmp(t->kind, "ui-text") == 0) {
            rp_ir_ui_text_t *x = &ir->ui_texts[ir->ui_text_count++];
            memset(x, 0, sizeof *x);
            parse_ui_text(&c, t, x);
        } else if (strcmp(t->kind, "dialog") == 0) {
            rp_ir_dialog_t *x = &ir->dialogs[ir->dialog_count++];
            memset(x, 0, sizeof *x);
            parse_dialog(&c, t, x);
        } else if (strcmp(t->kind, "dialog-control") == 0) {
            rp_ir_dialog_control_t *x = &ir->dialog_controls[ir->dialog_control_count++];
            memset(x, 0, sizeof *x);
            parse_dialog_control(&c, t, x);
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
    for (size_t i = 1; !c.nomem && i < ir->permission_count; ++i) {
        for (size_t j = i; j > 0 && cmp_str(ir->permissions[j - 1].id, ir->permissions[j].id) > 0; --j) {
            rp_ir_permission_t t = ir->permissions[j];
            ir->permissions[j] = ir->permissions[j - 1];
            ir->permissions[j - 1] = t;
        }
    }
    for (size_t i = 1; !c.nomem && i < ir->font_count; ++i) {
        for (size_t j = i; j > 0 && cmp_str(ir->fonts[j - 1].id, ir->fonts[j].id) > 0; --j) {
            rp_ir_font_t t = ir->fonts[j];
            ir->fonts[j] = ir->fonts[j - 1];
            ir->fonts[j - 1] = t;
        }
    }
    for (size_t i = 1; !c.nomem && i < ir->service_count; ++i) {
        for (size_t j = i; j > 0 && cmp_str(ir->services[j - 1].id, ir->services[j].id) > 0; --j) {
            rp_ir_service_t t = ir->services[j];
            ir->services[j] = ir->services[j - 1];
            ir->services[j - 1] = t;
        }
    }
    for (size_t i = 1; !c.nomem && i < ir->require_count; ++i) {
        for (size_t j = i; j > 0 && cmp_str(ir->requires[j - 1].id, ir->requires[j].id) > 0; --j) {
            rp_ir_require_t t = ir->requires[j];
            ir->requires[j] = ir->requires[j - 1];
            ir->requires[j - 1] = t;
        }
    }
    for (size_t i = 1; !c.nomem && i < ir->search_count; ++i) {
        for (size_t j = i; j > 0 && cmp_str(ir->searches[j - 1].id, ir->searches[j].id) > 0; --j) {
            rp_ir_search_t t = ir->searches[j];
            ir->searches[j] = ir->searches[j - 1];
            ir->searches[j - 1] = t;
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
#define SORT_BY_ID(arr, count, type)                                                                \
    for (size_t i = 1; !c.nomem && i < (count); ++i) {                                              \
        for (size_t j = i; j > 0 && cmp_str((arr)[j - 1].id, (arr)[j].id) > 0; --j) {               \
            type t = (arr)[j];                                                                      \
            (arr)[j] = (arr)[j - 1];                                                                \
            (arr)[j - 1] = t;                                                                       \
        }                                                                                           \
    }
    SORT_BY_ID(ir->assocs, ir->assoc_count, rp_ir_assoc_t)
    SORT_BY_ID(ir->protocols, ir->protocol_count, rp_ir_protocol_t)
    SORT_BY_ID(ir->msix_exts, ir->msix_ext_count, rp_ir_msix_ext_t)
#undef SORT_BY_ID
    for (size_t i = 1; !c.nomem && i < ir->registry_count; ++i) {
        for (size_t j = i; j > 0 && cmp_str(ir->registries[j - 1].id, ir->registries[j].id) > 0; --j) {
            rp_ir_registry_t t = ir->registries[j];
            ir->registries[j] = ir->registries[j - 1];
            ir->registries[j - 1] = t;
        }
    }
    if (!c.nomem && package) ui_checks(&c, uit, package);
    for (size_t i = 1; !c.nomem && i < ir->ui_text_count; ++i) {
        for (size_t j = i; j > 0 && cmp_str(ir->ui_texts[j - 1].id, ir->ui_texts[j].id) > 0; --j) {
            rp_ir_ui_text_t t = ir->ui_texts[j];
            ir->ui_texts[j] = ir->ui_texts[j - 1];
            ir->ui_texts[j - 1] = t;
        }
    }
    for (size_t i = 1; !c.nomem && i < ir->dialog_count; ++i) {
        for (size_t j = i; j > 0 && cmp_str(ir->dialogs[j - 1].id, ir->dialogs[j].id) > 0; --j) {
            rp_ir_dialog_t t = ir->dialogs[j];
            ir->dialogs[j] = ir->dialogs[j - 1];
            ir->dialogs[j - 1] = t;
        }
    }
    for (size_t i = 1; !c.nomem && i < ir->dialog_control_count; ++i) {
        for (size_t j = i; j > 0 && cmp_str(ir->dialog_controls[j - 1].id, ir->dialog_controls[j].id) > 0; --j) {
            rp_ir_dialog_control_t t = ir->dialog_controls[j];
            ir->dialog_controls[j] = ir->dialog_controls[j - 1];
            ir->dialog_controls[j - 1] = t;
        }
    }
    if (!c.nomem && opt->nfc) nfc_names(&c);
    if (!c.nomem) dialog_checks(&c);
    if (!c.nomem) class_checks(&c);
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

static void free_ltexts(proven_allocator_t a, rp_ir_ltext_t *x, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        rp_mem_free(a, x[i].lang);
        rp_mem_free(a, x[i].text);
    }
    rp_mem_free(a, x);
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
    for (size_t k = 0; k < ir->require_count; ++k) {
        rp_mem_free(a, ir->requires[k].id);
        rp_mem_free(a, ir->requires[k].condition);
        rp_mem_free(a, ir->requires[k].message);
    }
    rp_mem_free(a, ir->requires);
    for (size_t k = 0; k < ir->search_count; ++k) {
        rp_ir_search_t *x = &ir->searches[k];
        char *xs[] = { x->id, x->property, x->key, x->name, x->base, x->path, x->file_name, x->min_version, x->component_guid };
        for (size_t j = 0; j < sizeof xs / sizeof xs[0]; ++j) rp_mem_free(a, xs[j]);
    }
    rp_mem_free(a, ir->searches);
    for (size_t k = 0; k < ir->service_count; ++k) {
        rp_ir_service_t *x = &ir->services[k];
        char *xs[] = { x->id, x->file, x->name, x->display_name, x->description, x->args };
        for (size_t j = 0; j < sizeof xs / sizeof xs[0]; ++j) rp_mem_free(a, xs[j]);
    }
    rp_mem_free(a, ir->services);
    for (size_t k = 0; k < ir->font_count; ++k) {
        rp_mem_free(a, ir->fonts[k].id);
        rp_mem_free(a, ir->fonts[k].file);
        rp_mem_free(a, ir->fonts[k].title);
    }
    rp_mem_free(a, ir->fonts);
    for (size_t k = 0; k < ir->assoc_count; ++k) {
        rp_ir_assoc_t *x = &ir->assocs[k];
        char *xs[] = { x->id, x->extension, x->prog_id, x->description, x->target_file, x->icon_file, x->args };
        for (size_t j = 0; j < sizeof xs / sizeof xs[0]; ++j) rp_mem_free(a, xs[j]);
    }
    rp_mem_free(a, ir->assocs);
    for (size_t k = 0; k < ir->protocol_count; ++k) {
        rp_ir_protocol_t *x = &ir->protocols[k];
        char *xs[] = { x->id, x->name, x->description, x->target_file, x->args };
        for (size_t j = 0; j < sizeof xs / sizeof xs[0]; ++j) rp_mem_free(a, xs[j]);
    }
    rp_mem_free(a, ir->protocols);
    for (size_t k = 0; k < ir->msix_ext_count; ++k) {
        rp_ir_msix_ext_t *x = &ir->msix_exts[k];
        char *xs[] = { x->id, x->app, x->alias, x->task_id, x->display };
        for (size_t j = 0; j < sizeof xs / sizeof xs[0]; ++j) rp_mem_free(a, xs[j]);
    }
    rp_mem_free(a, ir->msix_exts);
    for (size_t k = 0; k < ir->permission_count; ++k) {
        rp_ir_permission_t *x = &ir->permissions[k];
        char *xs[] = { x->id, x->target, x->sddl, x->feature };
        for (size_t j = 0; j < sizeof xs / sizeof xs[0]; ++j) rp_mem_free(a, xs[j]);
    }
    rp_mem_free(a, ir->permissions);
    for (size_t k = 0; k < ir->ui_text_count; ++k) {
        rp_mem_free(a, ir->ui_texts[k].id);
        rp_mem_free(a, ir->ui_texts[k].text);
        free_ltexts(a, ir->ui_texts[k].by_lang, ir->ui_texts[k].by_lang_count);
    }
    rp_mem_free(a, ir->ui_texts);
    for (size_t k = 0; k < ir->ui_lang_count; ++k) {
        char *xs[] = { ir->ui_langs[k].name, ir->ui_langs[k].font, ir->ui_langs[k].license_source, ir->ui_langs[k].license_shown };
        for (size_t j = 0; j < sizeof xs / sizeof xs[0]; ++j) rp_mem_free(a, xs[j]);
    }
    for (size_t k = 0; k < ir->dialog_count; ++k) {
        char *xs[] = { ir->dialogs[k].id, ir->dialogs[k].title, ir->dialogs[k].description, ir->dialogs[k].after };
        for (size_t j = 0; j < sizeof xs / sizeof xs[0]; ++j) rp_mem_free(a, xs[j]);
        free_ltexts(a, ir->dialogs[k].title_by_lang, ir->dialogs[k].title_by_lang_count);
        free_ltexts(a, ir->dialogs[k].description_by_lang, ir->dialogs[k].description_by_lang_count);
    }
    rp_mem_free(a, ir->dialogs);
    for (size_t k = 0; k < ir->dialog_control_count; ++k) {
        rp_ir_dialog_control_t *x = &ir->dialog_controls[k];
        char *xs[] = { x->id, x->dialog, x->text, x->property };
        for (size_t j = 0; j < sizeof xs / sizeof xs[0]; ++j) rp_mem_free(a, xs[j]);
        for (size_t j = 0; j < x->value_count; ++j) {
            rp_mem_free(a, x->values[j]);
            rp_mem_free(a, x->labels[j]);
        }
        rp_mem_free(a, x->values);
        rp_mem_free(a, x->labels);
        free_ltexts(a, x->text_by_lang, x->text_by_lang_count);
        for (size_t j = 0; j < x->labels_by_lang_count; ++j) {
            for (size_t v = 0; v < x->value_count; ++v) rp_mem_free(a, x->labels_by_lang[j].labels[v]);
            rp_mem_free(a, x->labels_by_lang[j].labels);
            rp_mem_free(a, x->labels_by_lang[j].lang);
        }
        rp_mem_free(a, x->labels_by_lang);
    }
    rp_mem_free(a, ir->dialog_controls);
    char *us[] = { ir->license_source, ir->license_shown, ir->banner_source, ir->ui_install_dir };
    for (size_t k = 0; k < sizeof us / sizeof us[0]; ++k) rp_mem_free(a, us[k]);
    rp_mem_free(a, ir->removes);
    rp_mem_free(a, ir->copies);
    rp_mem_free(a, ir->properties);
    rp_mem_free(a, ir->actions);
    rp_mem_free(a, ir->arp_help);
    rp_mem_free(a, ir->arp_about);
    rp_mem_free(a, ir->msix_identity_name);
    rp_mem_free(a, ir->msix_publisher);
    rp_mem_free(a, ir->msix_publisher_display);
    rp_mem_free(a, ir->msix_min_version);
    for (size_t k = 0; k < ir->msix_app_count; ++k) {
        rp_ir_msix_app_t *x = &ir->msix_apps[k];
        rp_mem_free(a, x->id);
        rp_mem_free(a, x->exe);
        rp_mem_free(a, x->display);
        rp_mem_free(a, x->description);
        for (int i = 0; i < 3; ++i) {
            rp_mem_free(a, x->logo[i]);
            rp_mem_free(a, x->logo_path[i]);
        }
    }
    rp_mem_free(a, ir->msix_apps);
    for (size_t i = 0; i < ir->msix_block_count; ++i) {
        rp_mem_free(a, ir->msix_blocks[i].kind);
        rp_mem_free(a, ir->msix_blocks[i].id);
    }
    rp_mem_free(a, ir->msix_blocks);
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
    if (ir->scope) kv(&b, "scope", ir->scope == 1 ? "user" : "dual");      // only when set: older goldens stay
    if (ir->ui) {
        static const char *const sets[] = { "none", "basic", "minimal", "installdir", "features" };
        kv(&b, "ui", sets[ir->ui]);
        kv(&b, "license", ir->license_shown);
        kv(&b, "install-dir", ir->ui_install_dir);
    }
    if (ir->cab_external || ir->cab_max) {
        kv(&b, "cab", ir->cab_external ? "external" : "embed");
        snprintf(num, sizeof num, "%llu", (unsigned long long)(ir->cab_max >> 20));
        kv(&b, "cab-max-size", num);
    }
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
                if (d->guard) kv(&b, "guard", "1");
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
    static const char *const roots[] = { "HKMU", "HKCR", "HKCU", "HKLM" };     // index root + 1
    static const char *const rtypes[] = { "string", "expand", "dword", "binary", "multi", "qword" };
    for (size_t k = 0; k < ir->registry_count; ++k) {
        const rp_ir_registry_t *r = &ir->registries[k];
        rp_buf_puts(&b, "registry ");
        rp_buf_puts(&b, r->id);
        kv(&b, "root", roots[r->root + 1]);
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
    for (size_t k = 0; k < ir->require_count; ++k) {
        const rp_ir_require_t *r = &ir->requires[k];
        rp_buf_puts(&b, "require ");
        rp_buf_puts(&b, r->id);
        kv(&b, "condition", r->condition);
        kv(&b, "message", r->message);
        rp_buf_byte(&b, '\n');
    }
    static const char *const pkinds[] = { "dir", "file", "registry" };
    for (size_t k = 0; k < ir->permission_count; ++k) {
        const rp_ir_permission_t *x = &ir->permissions[k];
        rp_buf_puts(&b, "permission ");
        rp_buf_puts(&b, x->id);
        kv(&b, "kind", x->kind >= 0 && x->kind < 3 ? pkinds[x->kind] : "-");
        kv(&b, "target", x->target);
        kv(&b, "sddl", x->sddl);
        kv(&b, "feature", x->feature);
        rp_buf_byte(&b, '\n');
    }
    char lkey[32];
#define KV_LANG(base, lang, val)                                     \
    do {                                                             \
        snprintf(lkey, sizeof lkey, "%s-%s", (base), (lang));        \
        kv(&b, lkey, (val));                                         \
    } while (0)
    for (size_t k = 1; k < ir->ui_lang_count; ++k) {      // RFC-0012: English (0) is always there
        const rp_ir_ui_lang_t *L = &ir->ui_langs[k];
        char ids[RP_UI_LANGID_MAX * 6 + 1] = "";
        for (size_t j = 0; j < L->langid_count; ++j) {
            size_t n = strlen(ids);
            snprintf(ids + n, sizeof ids - n, "%s%u", j ? "," : "", (unsigned)L->langids[j]);
        }
        rp_buf_puts(&b, "ui-language ");
        rp_buf_puts(&b, L->code);
        kv(&b, "name", L->name);
        kv(&b, "font", L->font);
        kv(&b, "langids", ids);
        kv(&b, "license", L->license_shown);
        rp_buf_byte(&b, '\n');
    }
    for (size_t k = 0; k < ir->ui_text_count; ++k) {
        rp_buf_puts(&b, "ui-text ");
        rp_buf_puts(&b, ir->ui_texts[k].id);
        kv(&b, "text", ir->ui_texts[k].text);
        for (size_t j = 0; j < ir->ui_texts[k].by_lang_count; ++j) KV_LANG("text", ir->ui_texts[k].by_lang[j].lang, ir->ui_texts[k].by_lang[j].text);
        rp_buf_byte(&b, '\n');
    }
    for (size_t k = 0; k < ir->dialog_count; ++k) {
        rp_buf_puts(&b, "dialog ");
        rp_buf_puts(&b, ir->dialogs[k].id);
        kv(&b, "title", ir->dialogs[k].title);
        kv(&b, "description", ir->dialogs[k].description);
        kv(&b, "after", ir->dialogs[k].after);
        for (size_t j = 0; j < ir->dialogs[k].title_by_lang_count; ++j) KV_LANG("title", ir->dialogs[k].title_by_lang[j].lang, ir->dialogs[k].title_by_lang[j].text);
        for (size_t j = 0; j < ir->dialogs[k].description_by_lang_count; ++j)
            KV_LANG("description", ir->dialogs[k].description_by_lang[j].lang, ir->dialogs[k].description_by_lang[j].text);
        rp_buf_byte(&b, '\n');
    }
    for (size_t k = 0; k < ir->dialog_control_count; ++k) {
        static const char *const types[] = { "text", "checkbox", "edit", "radio", "combo" };
        const rp_ir_dialog_control_t *x = &ir->dialog_controls[k];
        char geo[64];
        rp_buf_puts(&b, "dialog-control ");
        rp_buf_puts(&b, x->id);
        kv(&b, "dialog", x->dialog);
        kv(&b, "type", x->type >= 0 && x->type < 5 ? types[x->type] : "-");
        snprintf(geo, sizeof geo, "%d,%d,%d,%d", x->x, x->y, x->width, x->height);
        kv(&b, "at", geo);
        kv(&b, "text", x->text);
        kv(&b, "property", x->property);
        for (size_t j = 0; j < x->value_count; ++j) {
            kv(&b, "value", x->values[j]);
            kv(&b, "label", x->labels[j]);
        }
        for (size_t j = 0; j < x->text_by_lang_count; ++j) KV_LANG("text", x->text_by_lang[j].lang, x->text_by_lang[j].text);
        for (size_t j = 0; j < x->labels_by_lang_count; ++j) {
            for (size_t v = 0; v < x->value_count; ++v) KV_LANG("label", x->labels_by_lang[j].lang, x->labels_by_lang[j].labels[v]);
        }
        rp_buf_byte(&b, '\n');
    }
    for (size_t k = 0; k < ir->font_count; ++k) {
        rp_buf_puts(&b, "font ");
        rp_buf_puts(&b, ir->fonts[k].id);
        kv(&b, "file", ir->fonts[k].file);
        kv(&b, "title", ir->fonts[k].title);
        rp_buf_byte(&b, '\n');
    }
    static const char *const starts[] = { "-", "-", "auto", "demand", "disabled" };
    static const char *const accounts[] = { "LocalSystem", "LocalService", "NetworkService" };
    for (size_t k = 0; k < ir->service_count; ++k) {
        const rp_ir_service_t *x = &ir->services[k];
        rp_buf_puts(&b, "service ");
        rp_buf_puts(&b, x->id);
        kv(&b, "file", x->file);
        kv(&b, "name", x->name);
        kv(&b, "display-name", x->display_name);
        kv(&b, "description", x->description);
        kv(&b, "start", starts[x->start]);
        kv(&b, "account", accounts[x->account]);
        kv(&b, "args", x->args);
        kv(&b, "start-on-install", x->start_on_install ? "1" : "0");
        rp_buf_byte(&b, '\n');
    }
    static const char *const skinds[] = { "registry", "file", "dir", "component" };
    static const char *const sroots[] = { "HKCR", "HKCU", "HKLM" };
    for (size_t k = 0; k < ir->search_count; ++k) {
        const rp_ir_search_t *x = &ir->searches[k];
        rp_buf_puts(&b, "search ");
        rp_buf_puts(&b, x->id);
        kv(&b, "property", x->property);
        kv(&b, "kind", skinds[x->kind]);
        if (x->kind == RP_SEARCH_REGISTRY) {
            kv(&b, "root", sroots[x->root]);
            kv(&b, "key", x->key);
            kv(&b, "name", x->name);
            kv(&b, "view", x->view32 ? "32" : "native");
        }
        kv(&b, "base", x->base);
        kv(&b, "path", x->path);
        kv(&b, "file", x->file_name);
        kv(&b, "min-version", x->min_version);
        kv(&b, "component-guid", x->component_guid);
        if (x->fills_dir) kv(&b, "fills-dir", "1");
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
    for (size_t k = 0; k < ir->assoc_count; ++k) {
        const rp_ir_assoc_t *x = &ir->assocs[k];
        rp_buf_puts(&b, "assoc ");
        rp_buf_puts(&b, x->id);
        kv(&b, "extension", x->extension);
        kv(&b, "prog-id", x->prog_id);
        kv(&b, "description", x->description);
        kv(&b, "target", x->target_file);
        kv(&b, "icon", x->icon_file);
        kv(&b, "args", x->args);
        rp_buf_byte(&b, '\n');
    }
    for (size_t k = 0; k < ir->protocol_count; ++k) {
        const rp_ir_protocol_t *x = &ir->protocols[k];
        rp_buf_puts(&b, "protocol ");
        rp_buf_puts(&b, x->id);
        kv(&b, "name", x->name);
        kv(&b, "description", x->description);
        kv(&b, "target", x->target_file);
        kv(&b, "args", x->args);
        rp_buf_byte(&b, '\n');
    }
    for (size_t k = 0; k < ir->msix_ext_count; ++k) {
        const rp_ir_msix_ext_t *x = &ir->msix_exts[k];
        rp_buf_puts(&b, "msix-extension ");
        rp_buf_puts(&b, x->id);
        kv(&b, "kind", x->kind == RP_MSIX_EXT_ALIAS ? "alias" : "startup-task");
        kv(&b, "app", x->app);
        kv(&b, "alias", x->alias);
        kv(&b, "task-id", x->task_id);
        kv(&b, "display-name", x->display);
        if (x->kind == RP_MSIX_EXT_STARTUP) kv(&b, "enabled", x->enabled ? "true" : "false");
        rp_buf_byte(&b, '\n');
    }
    return rp_buf_take(&b, out, len);
}
