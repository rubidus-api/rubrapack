// src/cli/edit.c - `rubrapack edit <file>.toml` (or .rpk) (RFC-0015): changes a source in place, through a
// menu of questions (the current value is the default) or with --set, --unset and --sync. Only
// the values it changes are rewritten: every other line - comments, order, hand-written tables -
// stays as it was. After each change the text is parsed again, so a change that would break the
// TOML is undone at once; saving checks the source like `lint`.

#include "rubrapack/buf.h"
#include "rubrapack/diag.h"
#include "rubrapack/inspect.h"
#include "rubrapack/mem.h"
#include "rubrapack/pal.h"
#include "rubrapack/toml.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "proven/heap.h"

#include "new_int.h"

// snprintf that may cut: the text is for messages and IDs checked afterwards.
#define CUT(out, cap, ...)                                                           \
    do {                                                                             \
        if (snprintf((out), (cap), __VA_ARGS__) >= (int)(cap)) (out)[(cap) - 1] = 0; \
    } while (0)

// ---- the text and its parse ----------------------------------------------------------------

typedef struct {
    const char *path;
    char       *t;          // NUL-terminated
    size_t      n;
    rp_tdoc_t   doc;
    bool        dirty;
} src_t;

static proven_allocator_t heap(void) { return proven_heap_allocator(); }

static bool reparse(src_t *s) {
    rp_toml_free(&s->doc);
    rp_srcdiags_t d = { 0 };
    return rp_toml_parse(heap(), (const uint8_t *)s->t, s->n, &s->doc, &d) == PROVEN_OK;
}

// Replaces t[from, to) with `ins`. Kept only when the result still parses.
static bool splice(src_t *s, size_t from, size_t to, const char *ins) {
    size_t k = strlen(ins), n = s->n - (to - from) + k;
    char *t = rp_mem_alloc(heap(), n + 1, 1);
    if (t == NULL) return false;
    memcpy(t, s->t, from);
    memcpy(t + from, ins, k);
    memcpy(t + from + k, s->t + to, s->n - to);
    t[n] = '\0';
    char *old = s->t;
    size_t old_n = s->n;
    s->t = t;
    s->n = n;
    if (reparse(s)) {
        rp_mem_free(heap(), old);
        s->dirty = true;
        return true;
    }
    rp_mem_free(heap(), t);
    s->t = old;
    s->n = old_n;
    (void)reparse(s);
    return false;
}

static size_t line_off(const src_t *s, uint32_t line) {
    size_t off = 0;
    for (uint32_t l = 1; l < line && off < s->n; ++off) {
        if (s->t[off] == '\n') ++l;
    }
    return off;
}

static size_t after_line(const src_t *s, size_t off) {
    while (off < s->n && s->t[off] != '\n') ++off;
    return off < s->n ? off + 1 : off;
}

// Where the value of a `key = value` line starts, and where it ends (strings, multi-line arrays).
static size_t value_start(const src_t *s, const rp_tkey_t *k) {
    size_t off = line_off(s, k->pos.line);
    while (off < s->n && s->t[off] != '=') ++off;
    if (off < s->n) ++off;
    while (off < s->n && (s->t[off] == ' ' || s->t[off] == '\t')) ++off;
    return off;
}

static size_t skip_string(const src_t *s, size_t i) {
    char q = s->t[i++];
    while (i < s->n && s->t[i] != q && s->t[i] != '\n') {
        if (q == '"' && s->t[i] == '\\' && i + 1 < s->n) ++i;
        ++i;
    }
    return i < s->n && s->t[i] == q ? i + 1 : i;
}

static size_t value_end(const src_t *s, size_t i) {
    if (i >= s->n) return i;
    if (s->t[i] == '"' || s->t[i] == '\'') return skip_string(s, i);
    if (s->t[i] == '[') {
        int depth = 0;
        while (i < s->n) {
            char c = s->t[i];
            if (c == '"' || c == '\'') {
                i = skip_string(s, i);
                continue;
            }
            if (c == '#') {
                while (i < s->n && s->t[i] != '\n') ++i;
                continue;
            }
            ++i;
            if (c == '[') ++depth;
            if (c == ']' && --depth == 0) return i;
        }
        return i;
    }
    while (i < s->n && !strchr(" \t#\r\n", s->t[i])) ++i;
    return i;
}

static rp_ttable_t *table(const src_t *s, const char *kind, const char *id) {
    for (size_t i = 0; i < s->doc.count; ++i) {
        rp_ttable_t *t = &s->doc.tables[i];
        if (strcmp(t->kind, kind) == 0 && ((id == NULL && t->id == NULL) || (id && t->id && strcmp(t->id, id) == 0))) return t;
    }
    return NULL;
}

static rp_tkey_t *key(rp_ttable_t *t, const char *k) {
    for (size_t i = 0; t && i < t->count; ++i) {
        if (strcmp(t->keys[i].key, k) == 0) return &t->keys[i];
    }
    return NULL;
}

// Points into the parse: valid only until the next change.
static const char *get(const src_t *s, const char *kind, const char *id, const char *k) {
    rp_tkey_t *x = key(table(s, kind, id), k);
    return x && x->val.kind == RP_TV_STRING ? x->val.str : NULL;
}

static void header(char *out, size_t cap, const char *kind, const char *id) {
    CUT(out, cap, "[%s%s%s]", kind, id ? "." : "", id ? id : "");
}

// Sets `key = literal` (a TOML value as text) in a table, making the key or the table as needed.
static bool set_raw(src_t *s, const char *kind, const char *id, const char *k, const char *literal) {
    rp_ttable_t *t = table(s, kind, id);
    char line[2400];
    if (t == NULL) {
        char h[200];
        header(h, sizeof h, kind, id);
        CUT(line, sizeof line, "%s%s\n%s = %s\n", s->n && s->t[s->n - 1] != '\n' ? "\n\n" : "\n", h, k, literal);
        return splice(s, s->n, s->n, line);
    }
    rp_tkey_t *x = key(t, k);
    if (x) {
        size_t v = value_start(s, x);
        return splice(s, v, value_end(s, v), literal);
    }
    size_t at = after_line(s, line_off(s, t->pos.line));
    if (t->count) {
        rp_tkey_t *last = &t->keys[t->count - 1];
        at = after_line(s, value_end(s, value_start(s, last)));
    }
    CUT(line, sizeof line, "%s = %s\n", k, literal);
    return splice(s, at, at, line);
}

static bool set_str(src_t *s, const char *kind, const char *id, const char *k, const char *value) {
    rp_buf_t b = rp_buf_new(heap(), 4096);
    rpn_toml_str(&b, value);
    uint8_t *lit = NULL;
    size_t len = 0;
    if (rp_buf_take(&b, &lit, &len) != PROVEN_OK) return false;
    char *z = rp_mem_alloc(heap(), len + 1, 1);
    bool ok = false;
    if (z) {
        memcpy(z, lit, len);
        z[len] = '\0';
        ok = set_raw(s, kind, id, k, z);
    }
    rp_mem_free(heap(), z);
    rp_mem_free(heap(), lit);
    return ok;
}

static bool unset(src_t *s, const char *kind, const char *id, const char *k) {
    rp_tkey_t *x = key(table(s, kind, id), k);
    if (x == NULL) return true;
    size_t from = line_off(s, x->pos.line);
    return splice(s, from, after_line(s, value_end(s, value_start(s, x))), "");
}

static bool remove_table(src_t *s, const char *kind, const char *id) {
    rp_ttable_t *t = table(s, kind, id);
    if (t == NULL) return true;
    size_t from = line_off(s, t->pos.line), to = s->n;
    for (size_t i = 0; i < s->doc.count; ++i) {
        size_t o = line_off(s, s->doc.tables[i].pos.line);
        if (o > from && o < to) to = o;
    }
    return splice(s, from, to, "");
}

// ---- helpers over the model ------------------------------------------------------------------

static void all_ids(const src_t *s, ids_t *ids) {
    ids->n = 0;
    for (size_t i = 0; i < s->doc.count && ids->n < sizeof ids->v / sizeof ids->v[0]; ++i) {
        if (s->doc.tables[i].id) CUT(ids->v[ids->n++], sizeof ids->v[0], "%s", s->doc.tables[i].id);
    }
}

// The program folder: the folder of the [file] sources (else of the [files] globs), or "dist".
static void find_dist(const src_t *s, char *out, size_t cap) {
    CUT(out, cap, "dist");
    for (size_t i = 0; i < s->doc.count; ++i) {
        const rp_ttable_t *t = &s->doc.tables[i];
        const char *v = strcmp(t->kind, "file") == 0 ? get(s, "file", t->id, "source") : strcmp(t->kind, "files") == 0 ? get(s, "files", t->id, "glob") : NULL;
        const char *slash = v ? strchr(v, '/') : NULL;
        if (slash && !strchr(slash + 1, '/') && strcmp(t->kind, "file") == 0) {
            CUT(out, cap, "%.*s", (int)(slash - v), v);
            return;
        }
    }
    for (size_t i = 0; i < s->doc.count; ++i) {
        const rp_ttable_t *t = &s->doc.tables[i];
        const char *v = strcmp(t->kind, "files") == 0 ? get(s, "files", t->id, "glob") : NULL;
        const char *slash = v ? strchr(v, '/') : NULL;
        if (slash) {
            CUT(out, cap, "%.*s", (int)(slash - v), v);
            return;
        }
    }
}

// A sub folder as `new` writes it: [dir.D] path = "INSTALLDIR/<name>" with a [files] glob
// "<dist>/<name>/**" into it.
typedef struct {
    char name[256], dir[64], files[64];
} sub_t;

static size_t sub_folders(const src_t *s, const char *dist, sub_t *out, size_t cap) {
    size_t n = 0;
    for (size_t i = 0; i < s->doc.count && n < cap; ++i) {
        const rp_ttable_t *t = &s->doc.tables[i];
        if (strcmp(t->kind, "files") != 0 || t->id == NULL) continue;
        const char *glob = get(s, "files", t->id, "glob"), *d = get(s, "files", t->id, "dir");
        size_t dl = strlen(dist);
        if (!glob || !d || strncmp(glob, dist, dl) != 0 || glob[dl] != '/') continue;
        const char *name = glob + dl + 1, *end = strstr(name, "/**");
        if (!end || end[3] != '\0' || memchr(name, '/', (size_t)(end - name))) continue;
        CUT(out[n].name, sizeof out[n].name, "%.*s", (int)(end - name), name);
        CUT(out[n].dir, sizeof out[n].dir, "%s", d);
        CUT(out[n].files, sizeof out[n].files, "%s", t->id);
        ++n;
    }
    return n;
}

static bool feature_used(const src_t *s, const char *f) {
    for (size_t i = 0; i < s->doc.count; ++i) {
        const rp_ttable_t *t = &s->doc.tables[i];
        if (strcmp(t->kind, "feature") == 0) {
            const char *p = get(s, "feature", t->id, "parent");
            if (p && strcmp(p, f) == 0) return true;
            continue;
        }
        const char *v = t->id ? get(s, t->kind, t->id, "feature") : NULL;
        if (v && strcmp(v, f) == 0) return true;
    }
    return false;
}

static void say(const char *text) { (void)rp_pal_puts(RP_OUT_STDERR, text); }

// ---- the menu items -------------------------------------------------------------------------

static int product(src_t *s, ans_t *a) {
    int rc;
    const char *v = get(s, "package", NULL, "name");
    if ((rc = rpn_ask(a, "Product name", v ? v : "", a->name, sizeof a->name, rpn_check_product))) return rc;
    set_str(s, "package", NULL, "name", a->name);
    v = get(s, "package", NULL, "manufacturer");
    if ((rc = rpn_ask(a, "Manufacturer", v ? v : "", a->manufacturer, sizeof a->manufacturer, rpn_check_product))) return rc;
    set_str(s, "package", NULL, "manufacturer", a->manufacturer);
    v = get(s, "package", NULL, "version");
    bool via_define = v && strcmp(v, "$(VERSION)") == 0 && get(s, "define", NULL, "VERSION");
    if (via_define) v = get(s, "define", NULL, "VERSION");
    if ((rc = rpn_ask(a, via_define ? "Version ([define] VERSION)" : "Version", v ? v : "1.0.0", a->version, sizeof a->version,
                      rpn_check_version))) return rc;
    set_str(s, via_define ? "define" : "package", NULL, via_define ? "VERSION" : "version", a->version);
    v = get(s, "package", NULL, "arch");
    if ((rc = rpn_ask(a, "Architecture (x64, x86, arm64)", v ? v : "x64", a->arch, sizeof a->arch, rpn_check_arch))) return rc;
    set_str(s, "package", NULL, "arch", a->arch);
    return 0;
}

static int install(src_t *s, ans_t *a) {
    int rc;
    const char *p = get(s, "dir", "INSTALLDIR", "path");
    if (p && strncmp(p, "ProgramFiles/", 13) == 0 && !strchr(p + 13, '/')) {
        if ((rc = rpn_ask(a, "Folder name under Program Files", p + 13, a->install_dir, sizeof a->install_dir, rpn_check_folder))) return rc;
        char path[300];
        CUT(path, sizeof path, "ProgramFiles/%s", a->install_dir);
        set_str(s, "dir", "INSTALLDIR", "path", path);
    } else if (p) {
        char path[512];
        if ((rc = rpn_ask(a, "Install folder ([dir.INSTALLDIR] path)", p, path, sizeof path, NULL))) return rc;
        set_str(s, "dir", "INSTALLDIR", "path", path);
    }
    const char *sc = get(s, "package", NULL, "scope");
    if ((rc = rpn_ask(a, "Install for (machine, user, dual)", sc ? sc : "machine", a->scope, sizeof a->scope, rpn_check_scope))) return rc;
    if (strcmp(a->scope, "machine") == 0) unset(s, "package", NULL, "scope");
    else set_str(s, "package", NULL, "scope", a->scope);
    return 0;
}

static int dialogs(src_t *s, ans_t *a) {
    int rc;
    const char *u = get(s, "package", NULL, "ui");
    if ((rc = rpn_ask(a, "Dialogs (none, basic, minimal, installdir, features)", u ? u : "none", a->ui, sizeof a->ui, rpn_check_ui))) return rc;
    if (strcmp(a->ui, "none") == 0) unset(s, "package", NULL, "ui");
    else set_str(s, "package", NULL, "ui", a->ui);
    if (strcmp(a->ui, "none") != 0 && strcmp(a->ui, "basic") != 0) {
        const char *l = get(s, "package", NULL, "license");
        if ((rc = rpn_ask(a, "License to accept (.txt, .md, .rtf; - for none)", l ? l : "-", a->license, sizeof a->license, rpn_check_license))) return rc;
        if (strcmp(a->license, "-") == 0) unset(s, "package", NULL, "license");
        else set_str(s, "package", NULL, "license", a->license);
    }
    if (strcmp(a->ui, "none") != 0) {
        rp_tkey_t *langs = key(table(s, "ui", NULL), "languages");
        bool ko = false, others = false;
        for (size_t i = 0; langs && langs->val.kind == RP_TV_ARRAY && i < langs->val.count; ++i) {
            if (langs->val.items[i].kind != RP_TV_STRING) continue;
            if (strcmp(langs->val.items[i].str, "ko") == 0) ko = true;
            else others = true;
        }
        bool want = ko;
        if ((rc = rpn_ask_yes(a, "Korean dialogs as well as English?", ko, &want))) return rc;
        if (want != ko) {
            if (others) say("  other languages in [ui] languages are kept; change that list with 7 (any key)\n");
            else if (want) set_raw(s, "ui", NULL, "languages", "[\"ko\"]");
            else unset(s, "ui", NULL, "languages");
        }
    }
    return 0;
}

// Once there is a feature, everything installed needs one (RP1202): dirs pass theirs on to the
// files, folders, INI and remove items in them; the rest - registry values not `with` a file,
// environment variables, items in a known folder - get Main themselves.
static void give_main(src_t *s) {
    static const char *const own[] = { "dir", "registry", "env", NULL };
    static const char *const via_dir[] = { "file", "files", "folder", "remove", "ini", NULL };
    for (size_t i = 0; i < s->doc.count; ++i) {
        const rp_ttable_t *t = &s->doc.tables[i];
        if (t->id == NULL || key((rp_ttable_t *)t, "feature")) continue;
        char kind[64], id[128];
        CUT(kind, sizeof kind, "%s", t->kind);
        CUT(id, sizeof id, "%s", t->id);
        bool need = false;
        for (size_t k = 0; own[k]; ++k) need |= strcmp(kind, own[k]) == 0;
        if (strcmp(kind, "registry") == 0 && key((rp_ttable_t *)t, "with")) need = false;
        for (size_t k = 0; via_dir[k]; ++k) {
            const char *d = strcmp(kind, via_dir[k]) == 0 ? get(s, kind, id, "dir") : NULL;
            if (d && table(s, "dir", d) == NULL) need = true;
        }
        if (need && set_str(s, kind, id, "feature", "Main")) i = (size_t)-1;   // the tables moved: start again
    }
}

static int optional_parts(src_t *s, ans_t *a) {
    char dist[512];
    find_dist(s, dist, sizeof dist);
    sub_t *subs = rp_mem_alloc(heap(), 256, sizeof *subs);
    if (subs == NULL) return RP_EXIT_IO;
    size_t n = sub_folders(s, dist, subs, 256);
    if (n == 0) {
        say("  the source has no sub folders as `new` writes them ([files] \"<folder>/<name>/**\"); add them with 6\n");
        rp_mem_free(heap(), subs);
        return 0;
    }
    const char *mainf = get(s, "dir", "INSTALLDIR", "feature");
    char cur[1024] = "";
    rpn_names_free(&a->scan.dirs);
    a->scan.dirs.v = rp_mem_alloc(heap(), n, sizeof(char *));
    for (size_t i = 0; a->scan.dirs.v && i < n; ++i) {
        size_t k = strlen(subs[i].name) + 1;
        a->scan.dirs.v[a->scan.dirs.n] = rp_mem_alloc(heap(), k, 1);
        if (a->scan.dirs.v[a->scan.dirs.n]) memcpy(a->scan.dirs.v[a->scan.dirs.n++], subs[i].name, k);
        const char *f = get(s, "dir", subs[i].dir, "feature");
        if (f && (!mainf || strcmp(f, mainf) != 0)) {
            size_t o = strlen(cur);
            CUT(cur + o, sizeof cur - o, "%s%s", o ? "," : "", subs[i].name);
        }
    }
    char line[1400];
    size_t o = (size_t)snprintf(line, sizeof line, "  sub folders:");
    for (size_t i = 0; i < n && o < sizeof line - 1; ++i) o += (size_t)snprintf(line + o, sizeof line - o, " %s", subs[i].name);
    if (o < sizeof line - 1) CUT(line + o, sizeof line - o, "\n");
    say(line);
    int rc = rpn_ask(a, "Optional parts the user may tick (sub folders, commas; - for none)", cur[0] ? cur : "-", a->optional,
                     sizeof a->optional, rpn_check_optional);
    if (rc) {
        rp_mem_free(heap(), subs);
        return rc;
    }
    bool any = strcmp(a->optional, "-") != 0;
    if (any && mainf == NULL) {                     // the first optional part: a required Main
        const char *nm = get(s, "package", NULL, "name");
        set_str(s, "feature", "Main", "title", nm ? nm : "Program");
        set_raw(s, "feature", "Main", "required", "true");
        give_main(s);
        mainf = "Main";
    }
    char mainf_copy[64];
    CUT(mainf_copy, sizeof mainf_copy, "%s", mainf ? mainf : "");
    static ids_t ids;
    for (size_t i = 0; i < n; ++i) {
        char want[1100];
        CUT(want, sizeof want, ",%s,", a->optional);
        char needle[300];
        CUT(needle, sizeof needle, ",%s,", subs[i].name);
        bool opt = any && strstr(want, needle) != NULL;
        const char *f = get(s, "dir", subs[i].dir, "feature");
        char old[64] = "";
        if (f) CUT(old, sizeof old, "%s", f);
        if (opt) {
            if (old[0] && strcmp(old, mainf_copy) != 0 && table(s, "feature", old)) {
                set_raw(s, "feature", old, "level", "2");
            } else {
                char id[64];
                all_ids(s, &ids);
                rpn_make_id(&ids, subs[i].name, "", id, sizeof id);
                set_str(s, "feature", id, "title", subs[i].name);
                set_raw(s, "feature", id, "level", "2");
                set_str(s, "dir", subs[i].dir, "feature", id);
            }
        } else if (mainf_copy[0]) {
            set_str(s, "dir", subs[i].dir, "feature", mainf_copy);
            if (old[0] && strcmp(old, mainf_copy) != 0 && !feature_used(s, old)) remove_table(s, "feature", old);
        }
    }
    rp_mem_free(heap(), subs);
    return 0;
}

// The [file] tables: their IDs and the base names of their sources.
static size_t file_tables(const src_t *s, char (*ids)[64], char (*names)[256], size_t cap) {
    size_t n = 0;
    for (size_t i = 0; i < s->doc.count && n < cap; ++i) {
        const rp_ttable_t *t = &s->doc.tables[i];
        const char *src = strcmp(t->kind, "file") == 0 && t->id ? get(s, "file", t->id, "source") : NULL;
        if (!src) continue;
        const char *base = strrchr(src, '/');
        CUT(ids[n], 64, "%s", t->id);
        CUT(names[n], 256, "%s", base ? base + 1 : src);
        ++n;
    }
    return n;
}

static const rp_ttable_t *shortcut_in(const src_t *s, const char *dir) {
    for (size_t i = 0; i < s->doc.count; ++i) {
        const rp_ttable_t *t = &s->doc.tables[i];
        const char *d = strcmp(t->kind, "shortcut") == 0 && t->id ? get(s, "shortcut", t->id, "dir") : NULL;
        if (d && strcmp(d, dir) == 0) return t;
    }
    return NULL;
}

static int shortcuts(src_t *s, ans_t *a) {
    static char ids[4096][64], names[4096][256];
    size_t n = file_tables(s, ids, names, 4096);
    if (n == 0) {
        say("  the source has no [file] tables for a shortcut to point at\n");
        return 0;
    }
    rpn_names_free(&a->scan.files);
    a->scan.files.v = rp_mem_alloc(heap(), n, sizeof(char *));
    for (size_t i = 0; a->scan.files.v && i < n; ++i) {
        size_t k = strlen(names[i]) + 1;
        a->scan.files.v[a->scan.files.n] = rp_mem_alloc(heap(), k, 1);
        if (a->scan.files.v[a->scan.files.n]) memcpy(a->scan.files.v[a->scan.files.n++], names[i], k);
    }
    const rp_ttable_t *start = shortcut_in(s, "Programs"), *desk = shortcut_in(s, "Desktop");
    char st_id[64] = "", dk_id[64] = "", cur[256] = "-";
    if (start) CUT(st_id, sizeof st_id, "%s", start->id);
    if (desk) CUT(dk_id, sizeof dk_id, "%s", desk->id);
    const char *target = start ? get(s, "shortcut", st_id, "target") : desk ? get(s, "shortcut", dk_id, "target") : NULL;
    for (size_t i = 0; i < n; ++i) {
        if (target && strncmp(target, "file:", 5) == 0 && strcmp(target + 5, ids[i]) == 0) CUT(cur, sizeof cur, "%s", names[i]);
    }
    for (size_t i = 0; strcmp(cur, "-") == 0 && !target && i < n; ++i) {
        if (rpn_ends_ci(names[i], ".exe")) CUT(cur, sizeof cur, "%s", names[i]);
    }
    int rc;
    if ((rc = rpn_ask(a, "Program the shortcuts open (- for no shortcuts)", cur, a->main, sizeof a->main, rpn_check_main))) return rc;
    bool want_start = false, want_desk = false;
    if (strcmp(a->main, "-") != 0) {
        if ((rc = rpn_ask_yes(a, "Start menu shortcut?", start != NULL || !desk, &want_start))) return rc;
        if ((rc = rpn_ask_yes(a, "Desktop shortcut?", desk != NULL, &want_desk))) return rc;
    }
    char tgt[80] = "";
    for (size_t i = 0; i < n; ++i) {
        if (strcmp(names[i], a->main) == 0) CUT(tgt, sizeof tgt, "file:%s", ids[i]);
    }
    char nm[256];                       // a copy: every change parses the text again
    CUT(nm, sizeof nm, "%s", get(s, "package", NULL, "name") ? get(s, "package", NULL, "name") : a->main);
    struct { bool want; char *id; const char *dir, *fresh; } w[] = { { want_start, st_id, "Programs", "StartMenu" },
                                                                   { want_desk, dk_id, "Desktop", "Desktop" } };
    for (int k = 0; k < 2; ++k) {
        if (!w[k].want) {
            if (w[k].id[0]) remove_table(s, "shortcut", w[k].id);
            continue;
        }
        if (!w[k].id[0]) {
            static ids_t used;
            all_ids(s, &used);
            char id[64];
            rpn_make_id(&used, w[k].fresh, "", id, sizeof id);
            CUT(w[k].id, 64, "%s", id);
            set_str(s, "shortcut", w[k].id, "dir", w[k].dir);
            set_str(s, "shortcut", w[k].id, "name", nm);
        }
        set_str(s, "shortcut", w[k].id, "target", tgt);
    }
    return 0;
}

// Brings the [file] and [files] tables in step with the program folder. ask = NULL: no questions.
static int sync_files(src_t *s, ans_t *a, bool ask) {
    char dist[512];
    find_dist(s, dist, sizeof dist);
    if (!a->scanned) {                  // lists that 4 and 5 made from the source, not a scan
        rpn_names_free(&a->scan.files);
        rpn_names_free(&a->scan.dirs);
    }
    const char *why = rpn_check_dist(a, dist);
    if (why) {
        char m[700];
        CUT(m, sizeof m, "  the program folder '%s': %s\n", dist, why);
        say(m);
        return ask ? 0 : RP_EXIT_IO;
    }
    static char ids[4096][64], names[4096][256];
    size_t n = file_tables(s, ids, names, 4096);
    sub_t *subs = rp_mem_alloc(heap(), 4096, sizeof *subs);
    if (subs == NULL) return RP_EXIT_IO;
    size_t ns = sub_folders(s, dist, subs, 4096);
    const char *mainf = get(s, "dir", "INSTALLDIR", "feature");
    char mainf_copy[64] = "";
    if (mainf) CUT(mainf_copy, sizeof mainf_copy, "%s", mainf);
    // Files that went away.
    for (size_t i = 0; i < n; ++i) {
        const char *src = get(s, "file", ids[i], "source");
        uint64_t size = 0;
        if (!src || rp_pal_stat(heap(), src, &size) == RP_FS_FILE) continue;
        bool target = false;
        char tgt[80];
        CUT(tgt, sizeof tgt, "file:%s", ids[i]);
        for (size_t t = 0; t < s->doc.count; ++t) {
            const char *v = s->doc.tables[t].id ? get(s, s->doc.tables[t].kind, s->doc.tables[t].id, "target") : NULL;
            target |= v && strcmp(v, tgt) == 0;
        }
        char m[700];
        if (target) {
            CUT(m, sizeof m, "  %s is gone, but a shortcut opens it: change the shortcuts (5) first\n", src);
            say(m);
            continue;
        }
        bool yes = true;
        CUT(m, sizeof m, "%s is gone. Remove [file.%s]?", src, ids[i]);
        if (ask && rpn_ask_yes(a, m, true, &yes)) break;
        if (yes) {
            remove_table(s, "file", ids[i]);
            CUT(m, sizeof m, "  removed [file.%s]\n", ids[i]);
            say(m);
        }
    }
    // Sub folders that went away.
    for (size_t i = 0; i < ns; ++i) {
        if (rpn_in_names(&a->scan.dirs, subs[i].name)) continue;
        char m[700];
        bool yes = true;
        CUT(m, sizeof m, "%s/%s is gone or empty. Remove [files.%s] and [dir.%s]?", dist, subs[i].name, subs[i].files, subs[i].dir);
        if (ask && rpn_ask_yes(a, m, true, &yes)) break;
        if (!yes) continue;
        remove_table(s, "files", subs[i].files);
        bool dir_used = false;
        for (size_t t = 0; t < s->doc.count; ++t) {
            const char *v = s->doc.tables[t].id ? get(s, s->doc.tables[t].kind, s->doc.tables[t].id, "dir") : NULL;
            dir_used |= v && strcmp(v, subs[i].dir) == 0;
        }
        if (!dir_used) remove_table(s, "dir", subs[i].dir);
        CUT(m, sizeof m, "  removed [files.%s]%s\n", subs[i].files, dir_used ? "" : " and its dir");
        say(m);
    }
    // New files and sub folders.
    static ids_t used;
    for (size_t i = 0; i < a->scan.files.n; ++i) {
        const char *f = a->scan.files.v[i];
        char src[800];
        CUT(src, sizeof src, "%s/%s", dist, f);
        bool known = false;
        for (size_t k = 0; k < n && !known; ++k) {
            const char *v = get(s, "file", ids[k], "source");
            known = v && strcmp(v, src) == 0;
        }
        if (known) continue;
        char m[900];
        bool yes = true;
        CUT(m, sizeof m, "%s is new. Add it?", src);
        if (ask && rpn_ask_yes(a, m, true, &yes)) break;
        if (!yes) continue;
        all_ids(s, &used);
        char id[64];
        rpn_make_id(&used, f, "", id, sizeof id);
        set_str(s, "file", id, "dir", "INSTALLDIR");
        set_str(s, "file", id, "source", src);
        CUT(m, sizeof m, "  added [file.%s]\n", id);
        say(m);
    }
    for (size_t i = 0; i < a->scan.dirs.n; ++i) {
        const char *f = a->scan.dirs.v[i];
        bool known = false;
        for (size_t k = 0; k < ns && !known; ++k) known = strcmp(subs[k].name, f) == 0;
        if (known) continue;
        char m[900];
        bool yes = true;
        CUT(m, sizeof m, "%s/%s/ is a new folder. Add it?", dist, f);
        if (ask && rpn_ask_yes(a, m, true, &yes)) break;
        if (!yes) continue;
        all_ids(s, &used);
        char did[64], fid[64], v[800];
        rpn_make_id(&used, f, "_dir", did, sizeof did);
        rpn_make_id(&used, f, "_files", fid, sizeof fid);
        CUT(v, sizeof v, "INSTALLDIR/%s", f);
        set_str(s, "dir", did, "path", v);
        if (mainf_copy[0]) set_str(s, "dir", did, "feature", mainf_copy);
        set_str(s, "files", fid, "dir", did);
        CUT(v, sizeof v, "%s/%s/**", dist, f);
        set_str(s, "files", fid, "glob", v);
        CUT(m, sizeof m, "  added [dir.%s] and [files.%s]\n", did, fid);
        say(m);
    }
    rp_mem_free(heap(), subs);
    return 0;
}

// `table.key=value`: the table is the part before the last dot of the name (package, dir.INSTALLDIR).
static bool split_name(const char *name, char *kind, char *id, char *k) {
    const char *dot = strrchr(name, '.');
    if (dot == NULL || dot == name || dot[1] == '\0' || strlen(name) > 190) return false;
    CUT(k, 64, "%s", dot + 1);
    char t[200];
    CUT(t, sizeof t, "%.*s", (int)(dot - name), name);
    char *d2 = strchr(t, '.');
    if (d2) {
        *d2 = '\0';
        CUT(id, 100, "%s", d2 + 1);
    } else {
        id[0] = '\0';
    }
    CUT(kind, 64, "%s", t);
    return kind[0] != '\0';
}

// A value as TOML when it is one ("2", true, ["ko"], "text"), else as a string.
static bool set_any(src_t *s, const char *kind, const char *id, const char *k, const char *value) {
    const char *idp = id[0] ? id : NULL;
    bool raw_ok = value[0] && (value[0] == '"' || value[0] == '\'' || value[0] == '[' || strcmp(value, "true") == 0 ||
                               strcmp(value, "false") == 0 || strspn(value, "0123456789-") == strlen(value));
    if (raw_ok && set_raw(s, kind, idp, k, value)) return true;
    if (value[0] == '"' || value[0] == '\'' || value[0] == '[') return false;   // meant as TOML, and not valid
    return set_str(s, kind, idp, k, value);
}

static int any_key(src_t *s, ans_t *a) {
    char name[200], value[1000];
    int rc;
    if ((rc = rpn_ask(a, "Key as table.key (package.version, dir.INSTALLDIR.path, ...)", "", name, sizeof name, NULL))) return rc;
    char kind[64], id[100], k[64];
    if (!split_name(name, kind, id, k)) {
        say("  give it as table.key, such as package.version or dir.INSTALLDIR.path\n");
        return 0;
    }
    const char *cur = get(s, kind, id[0] ? id : NULL, k);
    if ((rc = rpn_ask(a, "Value (text, a number, true/false, [\"a\", \"b\"]; - removes the key)", cur ? cur : "", value, sizeof value, NULL))) return rc;
    bool ok = strcmp(value, "-") == 0 ? unset(s, kind, id[0] ? id : NULL, k) : set_any(s, kind, id, k, value);
    if (!ok) say("  that does not fit the TOML subset; nothing was changed\n");
    return 0;
}

// Checks the text as it would be saved: written next to the source (sources are relative to it).
static int check(src_t *s, char *argv0) {
    char tmp[1100];
    CUT(tmp, sizeof tmp, "%s.rp-edit.toml", s->path);
    if (rp_pal_write_file_atomic(heap(), tmp, (const uint8_t *)s->t, s->n) != PROVEN_OK) {
        rp_diag_error(RP_DIAG_OUTPUT, "cannot write '%s' to check it", tmp);
        return RP_EXIT_IO;
    }
    char *lint_argv[] = { argv0, "lint", tmp, NULL };
    int rc = rp_cmd_lint_source(3, lint_argv);
    (void)rp_pal_remove_file(heap(), tmp);
    say(rc == RP_EXIT_OK ? "  lint: no problems\n" : "  lint: see the problems above (the file named there is a check copy)\n");
    return rc;
}

static int save(src_t *s, char *argv0) {
    if (rp_pal_write_file_atomic(heap(), s->path, (const uint8_t *)s->t, s->n) != PROVEN_OK) {
        rp_diag_error(RP_DIAG_OUTPUT, "cannot write '%s'", s->path);
        return RP_EXIT_IO;
    }
    s->dirty = false;
    char line[1200];
    CUT(line, sizeof line, "wrote %s\n", s->path);
    (void)rp_pal_puts(RP_OUT_STDOUT, line);
    char *lint_argv[] = { argv0, "lint", (char *)s->path, NULL };
    int rc = rp_cmd_lint_source(3, lint_argv);
    (void)rp_pal_puts(RP_OUT_STDOUT, rc == RP_EXIT_OK ? "lint: no problems\n" : "lint: see the problems above\n");
    return rc;
}

static void summary(const src_t *s) {
    const char *nm = get(s, "package", NULL, "name"), *v = get(s, "package", NULL, "version"), *ar = get(s, "package", NULL, "arch");
    if (v && strcmp(v, "$(VERSION)") == 0 && get(s, "define", NULL, "VERSION")) v = get(s, "define", NULL, "VERSION");
    const char *sc = get(s, "package", NULL, "scope"), *ui = get(s, "package", NULL, "ui");
    char line[1400];
    CUT(line, sizeof line,
             "\n%s: \"%s\" %s, %s, %s, dialogs %s%s\n"
             "  1 name, manufacturer, version, architecture   2 install folder, who it installs for\n"
             "  3 dialogs, license, languages                  4 optional parts\n"
             "  5 shortcuts                                    6 files: match the program folder\n"
             "  7 any key                                      v view   l lint   s save   q quit\n",
             s->path, nm ? nm : "?", v ? v : "?", ar ? ar : "?", sc ? sc : "machine", ui ? ui : "none", s->dirty ? " (changed)" : "");
    say(line);
}

static int usage(void) {
    rp_diag_error(RP_DIAG_EXTRA_ARGUMENT,
                  "usage: rubrapack edit <file>.toml | rubrapack edit <file>.toml [--set table.key=value]... [--unset table.key]... [--sync]");
    return RP_EXIT_USAGE;
}

int rp_cmd_edit(int argc, char **argv) {
    if (argc < 3 || argv[2][0] == '-') return usage();
    src_t s = { .path = argv[2] };
    uint8_t *data = NULL;
    size_t len = 0;
    proven_err_t err = rp_pal_read_file(heap(), s.path, (size_t)1 << 22, &data, &len);
    if (err != PROVEN_OK) {
        rp_diag_error(RP_DIAG_INPUT, err == PROVEN_ERR_NOT_FOUND ? "cannot find '%s'; make one with `rubrapack new %s`" : "cannot read '%s'", s.path, s.path);
        return RP_EXIT_IO;
    }
    if (len >= 2 && ((data[0] == 0xFF && data[1] == 0xFE) || (data[0] == 0xFE && data[1] == 0xFF))) {
        rp_diag_error(RP_DIAG_INPUT, "'%s' is UTF-16: edit changes UTF-8 sources only (save it as UTF-8 first)", s.path);
        rp_mem_free(heap(), data);
        return RP_EXIT_USAGE;
    }
    s.t = rp_mem_alloc(heap(), len + 1, 1);
    if (s.t == NULL) {
        rp_mem_free(heap(), data);
        return RP_EXIT_IO;
    }
    memcpy(s.t, data, len);
    s.t[len] = '\0';
    s.n = len;
    rp_mem_free(heap(), data);
    rp_srcdiags_t d = { 0 };
    if (rp_toml_parse(heap(), (const uint8_t *)s.t, s.n, &s.doc, &d) != PROVEN_OK) {
        rp_srcdiag_print(&d, s.path);
        rp_diag_error(RP_DIAG_INPUT, "'%s' is not a valid source (the TOML subset); fix it by hand first", s.path);
        rp_mem_free(heap(), s.t);
        return RP_EXIT_SOURCE;
    }
    ans_t *a = rp_mem_alloc(heap(), 1, sizeof *a);
    if (a == NULL) return RP_EXIT_IO;
    memset(a, 0, sizeof *a);
    int rc = RP_EXIT_OK;
    if (argc > 3) {
        // --set / --unset / --sync, in the order given, then save.
        for (int i = 3; i < argc && rc == RP_EXIT_OK; ++i) {
            char kind[64], id[100], k[64];
            if ((strcmp(argv[i], "--set") == 0 || strcmp(argv[i], "--unset") == 0) && i + 1 < argc) {
                bool set = argv[i][2] == 's';
                char name[200];
                const char *arg = argv[++i], *eq = set ? strchr(arg, '=') : NULL;
                CUT(name, sizeof name, "%.*s", (int)(eq ? (size_t)(eq - arg) : strlen(arg)), arg);
                if ((set && !eq) || !split_name(name, kind, id, k)) {
                    rc = usage();
                } else if (set ? !set_any(&s, kind, id, k, eq + 1) : !unset(&s, kind, id[0] ? id : NULL, k)) {
                    rp_diag_error(RP_DIAG_BAD_ARG_TEXT, "'%s' does not fit the TOML subset; nothing was saved", arg);
                    rc = RP_EXIT_USAGE;
                }
            } else if (strcmp(argv[i], "--sync") == 0) {
                rc = sync_files(&s, a, false);
            } else {
                rc = usage();
            }
        }
        if (rc == RP_EXIT_OK) rc = s.dirty ? save(&s, argv[0]) : (say("nothing changed\n"), RP_EXIT_OK);
    } else {
        for (bool done = false; !done && rc == RP_EXIT_OK;) {
            summary(&s);
            char c[16];
            rc = rpn_ask(a, "Choose", s.dirty ? "s" : "q", c, sizeof c, NULL);
            if (rc) break;
            switch (c[0]) {
            case '1': rc = product(&s, a); break;
            case '2': rc = install(&s, a); break;
            case '3': rc = dialogs(&s, a); break;
            case '4': rc = optional_parts(&s, a); break;
            case '5': rc = shortcuts(&s, a); break;
            case '6': rc = sync_files(&s, a, true); break;
            case '7': rc = any_key(&s, a); break;
            case 'v': (void)rp_pal_write(RP_OUT_STDOUT, (const uint8_t *)s.t, s.n); break;
            case 'l': (void)check(&s, argv[0]); break;
            case 's':
                rc = save(&s, argv[0]);
                done = true;
                break;
            case 'q': {
                bool leave = true;
                if (s.dirty && (rc = rpn_ask_yes(a, "Leave without saving the changes?", false, &leave))) break;
                done = leave;
                break;
            }
            default: say("  1-7, v, l, s or q\n");
            }
        }
    }
    if (a->scanned) rpn_scan_free(&a->scan);
    else {
        rpn_names_free(&a->scan.files);
        rpn_names_free(&a->scan.dirs);
    }
    rp_mem_free(heap(), a);
    rp_toml_free(&s.doc);
    rp_mem_free(heap(), s.t);
    return rc;
}
