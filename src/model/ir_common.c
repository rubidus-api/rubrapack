// src/model/ir_common.c - shared helpers of the .rpk model: names, keys, values, known folders.

#include "ir_int.h"

char *ir_dup_n(ctx_t *c, const char *s, size_t n) {
    char *r = rp_mem_alloc(c->alloc, n + 1, 1);
    if (r == NULL) {
        c->nomem = true;
        return NULL;
    }
    memcpy(r, s, n);
    r[n] = '\0';
    return r;
}

char *ir_dup(ctx_t *c, const char *s) { return s ? ir_dup_n(c, s, strlen(s)) : NULL; }

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

const char *ir_suggest(const char *word, const char *const *choices) {
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

bool ir_valid_id(const char *s, size_t max) {
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

bool ir_check_id(ctx_t *c, const rp_ttable_t *t, size_t max) {
    if (!ir_valid_id(t->id, max)) {
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
const char *ir_lang_of_key(const char *key, const char *base) {
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

// Like ir_check_keys, and also takes `base-xx` for every base in `lang_bases` (NULL-terminated).
// An .ico named by a key (RFC-0013 A1): read at build time into the Icon table.

// `when` (RFC-0013 A2): an MSI condition for a feature or a component.

void ir_check_keys_lang(ctx_t *c, const rp_ttable_t *t, const char *const *allowed, const char *const *lang_bases) {
    for (size_t k = 0; k < t->count; ++k) {
        bool known = false;
        for (size_t j = 0; allowed[j]; ++j) known |= strcmp(t->keys[k].key, allowed[j]) == 0;
        for (size_t j = 0; lang_bases && lang_bases[j]; ++j) known |= ir_lang_of_key(t->keys[k].key, lang_bases[j]) != NULL;
        // msi-only (RFC-0009 M6) is read for every item table outside [msix-*] (msix_blockers).
        known |= t->id && strncmp(t->kind, "msix", 4) != 0 && strcmp(t->keys[k].key, "msi-only") == 0;
        if (!known) {
            const char *hint = ir_suggest(t->keys[k].key, allowed);
            ERR(c, t->keys[k].pos, "RP1201", "unknown key '%s' in [%s%s%s]%s%s%s", t->keys[k].key, t->kind, t->id ? "." : "",
                t->id ? t->id : "", hint ? " (did you mean '" : "", hint ? hint : "", hint ? "'?)" : "");
        }
    }
}

void ir_check_keys(ctx_t *c, const rp_ttable_t *t, const char *const *allowed) { ir_check_keys_lang(c, t, allowed, NULL); }

const rp_tkey_t *ir_find_key(const rp_ttable_t *t, const char *key) {
    for (size_t k = 0; k < t->count; ++k) {
        if (strcmp(t->keys[k].key, key) == 0) return &t->keys[k];
    }
    return NULL;
}

bool ir_msix_output(const ctx_t *c) {
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
        const rp_tkey_t *a = ir_find_key(t, "arch");
        if (a && a->val.kind == RP_TV_STRING && memchr(a->val.str, '$', a->val.len) == NULL) return a->val.str;
    }
    return NULL;
}

// $(NAME) substitution, once (RFC-0002 5): -D first, then [define], then the built-in $(ARCH);
// values are not re-read.
char *ir_subst(ctx_t *c, const rp_tval_t *v) {
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
                const rp_tkey_t *k = ir_find_key(c->define, name);
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
    char *r = ir_dup_n(c, (const char *)out, n);
    rp_mem_free(c->alloc, out);
    return r;
}

// String value of a key (after substitution); *present tells whether the key was there.
char *ir_get_str(ctx_t *c, const rp_ttable_t *t, const char *key, bool required, bool *present) {
    const rp_tkey_t *k = ir_find_key(t, key);
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
    char *s = ir_subst(c, &k->val);
    if (s && s[0] == '\0' && required) {
        ERR(c, k->pos, "RP1305", "'%s' must not be empty", key);
        rp_mem_free(c->alloc, s);
        return NULL;
    }
    return s;
}

// Every `base-xx` string of the table (RFC-0012), in source order.
void ir_get_ltexts(ctx_t *c, const rp_ttable_t *t, const char *base, rp_ir_ltext_t **out, size_t *count) {
    size_t n = 0;
    *out = NULL;
    *count = 0;
    for (size_t k = 0; k < t->count; ++k) n += ir_lang_of_key(t->keys[k].key, base) != NULL;
    if (n == 0) return;
    *out = rp_mem_alloc(c->alloc, n, sizeof **out);
    if (*out == NULL) {
        c->nomem = true;
        return;
    }
    for (size_t k = 0; k < t->count; ++k) {
        const char *code = ir_lang_of_key(t->keys[k].key, base);
        if (code == NULL) continue;
        if (t->keys[k].val.kind != RP_TV_STRING) {
            ERR(c, t->keys[k].pos, "RP1306", "'%s' must be a string", t->keys[k].key);
            continue;
        }
        (*out)[*count] = (rp_ir_ltext_t){ ir_dup(c, code), ir_subst(c, &t->keys[k].val) };
        ++*count;
    }
}

bool ir_get_bool(ctx_t *c, const rp_ttable_t *t, const char *key, bool dflt) {
    const rp_tkey_t *k = ir_find_key(t, key);
    if (k == NULL) return dflt;
    if (k->val.kind != RP_TV_BOOL) {
        ERR(c, k->pos, "RP1306", "'%s' must be true or false", key);
        return dflt;
    }
    return k->val.b;
}

int64_t ir_get_int(ctx_t *c, const rp_ttable_t *t, const char *key, int64_t dflt, int64_t lo, int64_t hi) {
    const rp_tkey_t *k = ir_find_key(t, key);
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

rp_pos_t ir_key_pos(const rp_ttable_t *t, const char *key) {
    const rp_tkey_t *k = ir_find_key(t, key);
    return k ? k->pos : t->pos;
}

// ---- value checks -------------------------------------------------------------------------

bool ir_is_hex(char ch) { return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F'); }

// {8-4-4-4-12}, normalised to upper case in place.
bool ir_guid_ok(char *s) {
    static const int groups[5] = { 8, 4, 4, 4, 12 };
    if (strlen(s) != 38 || s[0] != '{' || s[37] != '}') return false;
    size_t p = 1;
    for (int g = 0; g < 5; ++g) {
        for (int k = 0; k < groups[g]; ++k, ++p) {
            if (!ir_is_hex(s[p])) return false;
            if (s[p] >= 'a' && s[p] <= 'f') s[p] = (char)(s[p] - 32);
        }
        if (g < 4 && s[p++] != '-') return false;
    }
    return true;
}

bool ir_ascii_only(const char *s) {
    for (; *s; ++s) {
        if ((unsigned char)*s >= 0x80) return false;
    }
    return true;
}

bool ir_has_control(const char *s) {
    for (; *s; ++s) {
        if ((unsigned char)*s < 0x20 || (unsigned char)*s == 0x7F) return true;
    }
    return false;
}

// RFC-0002 8.2 / RFC-0001 14.3: one target path component.
bool ir_target_name_ok(ctx_t *c, const char *name, rp_pos_t pos) {
    size_t n = strlen(name);
    const char *why = NULL;
    if (n == 0) why = "is empty";
    else if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) why = "is '.' or '..'";
    else if (ir_has_control(name)) why = "contains a control character";
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
void ir_fold(const char *s, char *out, size_t cap) {
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

bool ir_known_folder(const char *s) {
    for (size_t k = 0; known_folders[k]; ++k) {
        if (strcmp(s, known_folders[k]) == 0) return true;
    }
    return false;
}
