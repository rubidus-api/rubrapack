// src/model/ir_files.c - [package], [feature], [dir], [file], [files.*] wildcards and [folder].

#include "ir_int.h"
#include "rubrapack/cab.h"

rp_ir_dir_t *ir_push_dir(ctx_t *c) {
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

rp_ir_file_t *ir_push_file(ctx_t *c) {
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
bool ir_parse_version(const char *s, uint16_t parts[4], size_t *count) {
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

void ir_parse_package(ctx_t *c, const rp_ttable_t *t) {
    static const char *const keys[] = { "name", "summary-name", "manufacturer", "version", "arch", "upgrade-code",
                                        "upgrade-code-x64", "upgrade-code-arm64", "upgrade-code-x86",
                                        "product-code", "scope", "language", "ui", "license", "icon", "reboot",
                                        "downgrade-message", "compress", "cab", "cab-max-size", "refuse-upgrade-below",
                                        "refuse-upgrade-message", "cleanup", "parent", "remove-addons", "replaces", NULL };
    rp_ir_t *ir = c->ir;
    ir_check_keys(c, t, keys);
    ir->name = ir_get_str(c, t, "name", true, NULL);
    ir->manufacturer = ir_get_str(c, t, "manufacturer", true, NULL);
    ir->summary_name = ir_get_str(c, t, "summary-name", false, NULL);
    if (ir->summary_name && !ir_ascii_only(ir->summary_name)) {
        ERR(c, ir_key_pos(t, "summary-name"), "RP1308", "summary-name must be ASCII (summary information holds ASCII only)");
    }
    if (ir->name && ir_has_control(ir->name)) ERR(c, ir_key_pos(t, "name"), "RP1308", "name contains a control character");

    ir->version = ir_get_str(c, t, "version", true, NULL);
    if (ir->version && !ir_parse_version(ir->version, ir->version_parts, &ir->version_count)) {
        ERR(c, ir_key_pos(t, "version"), "RP1308",
            "version '%s' must be a.b.c or a.b.c.d with a, b <= 255 and c, d <= 65535", ir->version);
    }
    // Older versions that must be removed by hand first (RFC-0003 section 9, T1): the upgrade is
    // refused and the message names the removal command.
    ir->refuse_below = ir_get_str(c, t, "refuse-upgrade-below", false, NULL);
    ir->refuse_message = ir_get_str(c, t, "refuse-upgrade-message", false, NULL);
    if (ir->refuse_below) {
        uint16_t below[4] = { 0 };
        size_t bn = 0;
        if (!ir_parse_version(ir->refuse_below, below, &bn)) {
            ERR(c, ir_key_pos(t, "refuse-upgrade-below"), "RP1308", "refuse-upgrade-below '%s' must be a version like 1.2.3",
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
                ERR(c, ir_key_pos(t, "refuse-upgrade-below"), "RP1314",
                    "refuse-upgrade-below %s is above this package's version %s", ir->refuse_below, ir->version);
            }
        }
    } else if (ir->refuse_message) {
        ERR(c, ir_key_pos(t, "refuse-upgrade-message"), "RP1314", "refuse-upgrade-message needs refuse-upgrade-below");
    }

    // The source's arch is its home architecture; --arch may build another one (DECISIONS
    // 2026-09-26 "Upgrade family per architecture").
    char *home = ir_get_str(c, t, "arch", true, NULL);
    char *arch = c->opt->arch ? ir_dup(c, c->opt->arch) : (home ? ir_dup(c, home) : NULL);
    if (arch) {
        if (strcmp(arch, "x64") == 0) ir->arch = RP_ARCH_X64;
        else if (strcmp(arch, "arm64") == 0) ir->arch = RP_ARCH_ARM64;
        else if (strcmp(arch, "x86") == 0) ir->arch = RP_ARCH_X86;
        else ERR(c, ir_key_pos(t, "arch"), "RP1308", "arch must be \"x64\", \"arm64\" or \"x86\" (got '%s')", arch);
    }
    if (home && strcmp(home, "x64") != 0 && strcmp(home, "arm64") != 0 && strcmp(home, "x86") != 0) {
        ERR(c, ir_key_pos(t, "arch"), "RP1308", "arch must be \"x64\", \"arm64\" or \"x86\" (got '%s')", home);
    }

    ir->upgrade_code = ir_get_str(c, t, "upgrade-code", true, NULL);
    if (ir->upgrade_code && !ir_guid_ok(ir->upgrade_code)) {
        ERR(c, ir_key_pos(t, "upgrade-code"), "RP1308", "upgrade-code must be a GUID like {12345678-1234-1234-1234-123456789ABC}");
    }
    // Each architecture is its own upgrade family: a build for another architecture than the
    // source's own needs upgrade-code-<arch>, so one UpgradeCode never spans two architectures.
    char code_key[32];
    snprintf(code_key, sizeof code_key, "upgrade-code-%s", arch ? arch : "");
    char *own = arch ? ir_get_str(c, t, code_key, false, NULL) : NULL;
    if (own && !ir_guid_ok(own)) ERR(c, ir_key_pos(t, code_key), "RP1308", "%s must be a GUID", code_key);
    if (own) {
        if (ir->upgrade_code && strcmp(own, ir->upgrade_code) == 0 && home && arch && strcmp(home, arch) != 0) {
            ERR(c, ir_key_pos(t, code_key), "RP1309", "%s must differ from upgrade-code (one upgrade family per architecture)", code_key);
        }
        rp_mem_free(c->alloc, ir->upgrade_code);
        ir->upgrade_code = own;
    } else if (home && arch && strcmp(home, arch) != 0 && !ir_msix_output(c)) {   // an MSIX has no upgrade code
        ERR(c, ir_key_pos(t, "arch"), "RP1309",
            "building %s from a %s source needs its own upgrade family: add %s = \"{...}\" to [package]", arch, home, code_key);
    }
    for (int k = 0; k < 3; ++k) {       // the others still have to be GUIDs, and distinct
        static const char *const others[] = { "upgrade-code-x64", "upgrade-code-arm64", "upgrade-code-x86" };
        const rp_tkey_t *key = ir_find_key(t, others[k]);
        if (key && key->val.kind == RP_TV_STRING && strcmp(others[k], code_key) != 0) {
            char *g = ir_get_str(c, t, others[k], false, NULL);
            if (g && !ir_guid_ok(g)) ERR(c, key->pos, "RP1308", "%s must be a GUID", others[k]);
            rp_mem_free(c->alloc, g);
        }
    }
    rp_mem_free(c->alloc, home);
    rp_mem_free(c->alloc, arch);
    ir->product_code = ir_get_str(c, t, "product-code", false, NULL);
    if (ir->product_code && !ir_guid_ok(ir->product_code)) {
        ERR(c, ir_key_pos(t, "product-code"), "RP1308", "product-code must be a GUID");
    }

    char *scope = ir_get_str(c, t, "scope", false, NULL);
    if (scope && strcmp(scope, "user") == 0) ir->scope = 1;
    else if (scope && strcmp(scope, "dual") == 0) ir->scope = 2;
    else if (scope && strcmp(scope, "machine") != 0) {
        ERR(c, ir_key_pos(t, "scope"), "RP1308", "scope must be \"machine\", \"user\" or \"dual\"");
    }
    rp_mem_free(c->alloc, scope);

    ir->language = 1033;
    char *lang = ir_get_str(c, t, "language", false, NULL);
    if (lang) {
        if (strcmp(lang, "ko-KR") == 0) ir->language = 1042;
        else if (strcmp(lang, "en-US") != 0) ERR(c, ir_key_pos(t, "language"), "RP1308", "language must be \"ko-KR\" or \"en-US\"");
        rp_mem_free(c->alloc, lang);
    }

    char *ui = ir_get_str(c, t, "ui", false, NULL);
    static const char *const sets[] = { "none", "basic", "minimal", "installdir", "features" };
    for (int k = 0; ui && k < 5; ++k) {
        if (strcmp(ui, sets[k]) == 0) {
            ir->ui = k;
            rp_mem_free(c->alloc, ui);
            ui = NULL;
        }
    }
    if (ui) ERR(c, ir_key_pos(t, "ui"), "RP1316", "ui must be none, basic, minimal, installdir or features (got '%s')", ui);
    rp_mem_free(c->alloc, ui);
    ir->license_shown = ir_get_str(c, t, "license", false, NULL);      // checked in ir_ui_checks (RFC-0005 K3)
    if (ir_find_key(t, "icon")) ERR(c, ir_key_pos(t, "icon"), "RP1316", "the icon of the installed apps list is [arp] icon = \"app.ico\"");

    ir->reboot_suppress = true;
    char *reboot = ir_get_str(c, t, "reboot", false, NULL);
    if (reboot) {
        if (strcmp(reboot, "allow") == 0) ir->reboot_suppress = false;
        else if (strcmp(reboot, "suppress") != 0) ERR(c, ir_key_pos(t, "reboot"), "RP1308", "reboot must be \"suppress\" or \"allow\"");
        rp_mem_free(c->alloc, reboot);
    }
    ir->no_cleanup = !ir_get_bool(c, t, "cleanup", true);     // RFC-0026
    // x46 (jamotong): an add-on names its main product; the main product's real removal removes the
    // add-ons through its cleanup task. MSI only.
    ir->parent = ir_get_str(c, t, "parent", false, NULL);
    if (ir->parent && !ir_guid_ok(ir->parent)) {
        ERR(c, ir_key_pos(t, "parent"), "RP1308", "parent must be the main product's upgrade-code, a GUID like {12345678-1234-1234-1234-123456789ABC}");
    } else if (ir->parent && ir->upgrade_code && strcmp(ir->parent, ir->upgrade_code) == 0) {
        ERR(c, ir_key_pos(t, "parent"), "RP1309", "parent must be another product's upgrade-code, not this package's own");
    }
    ir->remove_addons = ir_get_bool(c, t, "remove-addons", false);
    if (ir->remove_addons && ir->no_cleanup) {
        ERR(c, ir_key_pos(t, "remove-addons"), "RP1314", "remove-addons needs the cleanup task: remove cleanup = false");
    }
    // x47 (jamotong): products this package takes the place of - removed, any version, as it installs.
    const rp_tkey_t *rk = ir_find_key(t, "replaces");
    if (rk && rk->val.kind != RP_TV_ARRAY) {
        ERR(c, rk->pos, "RP1306", "replaces is an array of upgrade codes, like [\"{12345678-1234-1234-1234-123456789ABC}\"]");
    }
    for (size_t k = 0; rk && rk->val.kind == RP_TV_ARRAY && k < rk->val.count; ++k) {
        const rp_tval_t *v = &rk->val.items[k];
        char *code = v->kind == RP_TV_STRING ? ir_subst(c, v) : NULL;
        if (code == NULL || !ir_guid_ok(code)) {
            ERR(c, rk->pos, "RP1308", "replaces holds upgrade codes, GUIDs like {12345678-1234-1234-1234-123456789ABC} (got '%s')", code ? code : "?");
        } else if ((ir->upgrade_code && strcmp(code, ir->upgrade_code) == 0) || (ir->parent && strcmp(code, ir->parent) == 0)) {
            ERR(c, rk->pos, "RP1309", "replaces names %s, which is this package's own upgrade-code or its parent", code);
        } else if (ir->replace_count == 16) {
            ERR(c, rk->pos, "RP1308", "replaces holds at most 16 upgrade codes");
        } else {
            bool dup = false;
            for (size_t j = 0; j < ir->replace_count; ++j) dup = dup || strcmp(ir->replaces[j], code) == 0;
            if (dup) ERR(c, rk->pos, "RP1301", "replaces lists %s twice", code);
            else memcpy(ir->replaces[ir->replace_count++], code, 39);
        }
        rp_mem_free(c->alloc, code);
    }
    if ((ir->parent || ir->remove_addons || ir->replace_count) && ir_msix_output(c)) {
        const char *key = ir->parent ? "parent" : ir->remove_addons ? "remove-addons" : "replaces";
        ERR(c, ir_key_pos(t, key), "RP1316", "%s is for MSI packages: an MSIX is not removed by msiexec", key);
    }
    ir->downgrade_message = ir_get_str(c, t, "downgrade-message", false, NULL);

    char *comp = c->opt->compress ? ir_dup(c, c->opt->compress) : ir_get_str(c, t, "compress", false, NULL);
    ir->compress = 6;
    if (comp) {
        if (strcmp(comp, "none") == 0) ir->compress = -1;
        else if (strcmp(comp, "mszip") == 0) ir->compress = 6;
        else if (strncmp(comp, "mszip:", 6) == 0 && comp[6] >= '0' && comp[6] <= '9' && comp[7] == '\0') ir->compress = comp[6] - '0';
        else if (strcmp(comp, "lzx") == 0) ir->compress = RP_CAB_LZX(21);
        else if (strncmp(comp, "lzx:", 4) == 0 && comp[4] >= '1' && comp[4] <= '2' && comp[5] >= '0' && comp[5] <= '9' && comp[6] == '\0' &&
                 (comp[4] - '0') * 10 + (comp[5] - '0') >= 15 && (comp[4] - '0') * 10 + (comp[5] - '0') <= 21) {
            ir->compress = RP_CAB_LZX((comp[4] - '0') * 10 + (comp[5] - '0'));
        } else {
            ERR(c, ir_key_pos(t, "compress"), "RP1308", "compress must be \"none\", \"mszip\", \"mszip:0\" ... \"mszip:9\", \"lzx\" or \"lzx:15\" ... \"lzx:21\"");
        }
        rp_mem_free(c->alloc, comp);
    }
    char *cab = ir_get_str(c, t, "cab", false, NULL);
    if (cab && strcmp(cab, "external") == 0) {
        ir->cab_external = true;
    } else if (cab && strcmp(cab, "embed") != 0) {
        ERR(c, ir_key_pos(t, "cab"), "RP1308", "cab must be \"embed\" or \"external\"");
    }
    rp_mem_free(c->alloc, cab);
    // Split: at most this many MiB of (uncompressed) files per cabinet; a larger file gets its own.
    ir->cab_max = (uint64_t)ir_get_int(c, t, "cab-max-size", 0, 1, 2047) << 20;
}

// [module]: a merge module (RFC-0017). Its GUID takes the upgrade code's place: component GUIDs
// derive from it, and every key of the module carries it.
void ir_parse_module(ctx_t *c, const rp_ttable_t *t) {
    static const char *const keys[] = { "name", "manufacturer", "version", "arch", "id", "language", "compress", NULL };
    rp_ir_t *ir = c->ir;
    ir_check_keys(c, t, keys);
    ir->module = true;
    ir->name = ir_get_str(c, t, "name", true, NULL);
    // ModuleSignature.ModuleID is <name>.<GUID>, at most 72 characters.
    if (ir->name && (!ir_valid_id(ir->name, 35) || strchr(ir->name, '.'))) {
        ERR(c, ir_key_pos(t, "name"), "RP1308", "a module's name is its ID in the ModuleID: letters, digits and _, at most 35 characters");
    }
    ir->manufacturer = ir_get_str(c, t, "manufacturer", true, NULL);
    ir->version = ir_get_str(c, t, "version", true, NULL);
    if (ir->version && !ir_parse_version(ir->version, ir->version_parts, &ir->version_count)) {
        ERR(c, ir_key_pos(t, "version"), "RP1308", "version '%s' must be a.b.c or a.b.c.d", ir->version);
    }
    char *arch = c->opt->arch ? ir_dup(c, c->opt->arch) : ir_get_str(c, t, "arch", true, NULL);
    if (arch) {
        if (strcmp(arch, "x64") == 0) ir->arch = RP_ARCH_X64;
        else if (strcmp(arch, "arm64") == 0) ir->arch = RP_ARCH_ARM64;
        else if (strcmp(arch, "x86") == 0) ir->arch = RP_ARCH_X86;
        else ERR(c, ir_key_pos(t, "arch"), "RP1308", "arch must be \"x64\", \"arm64\" or \"x86\" (got '%s')", arch);
        rp_mem_free(c->alloc, arch);
    }
    ir->upgrade_code = ir_get_str(c, t, "id", true, NULL);
    if (ir->upgrade_code && !ir_guid_ok(ir->upgrade_code)) {
        ERR(c, ir_key_pos(t, "id"), "RP1308", "id must be a GUID like {12345678-1234-1234-1234-123456789ABC}: the module's own, kept in every version");
    }
    ir->language = 0;               // language neutral, as most modules are
    char *lang = ir_get_str(c, t, "language", false, NULL);
    if (lang) {
        if (strcmp(lang, "ko-KR") == 0) ir->language = 1042;
        else if (strcmp(lang, "en-US") == 0) ir->language = 1033;
        else if (strcmp(lang, "neutral") != 0) ERR(c, ir_key_pos(t, "language"), "RP1308", "language must be \"neutral\", \"en-US\" or \"ko-KR\"");
        rp_mem_free(c->alloc, lang);
    }
    ir->reboot_suppress = true;
    ir->compress = 6;
    char *comp = c->opt->compress ? ir_dup(c, c->opt->compress) : ir_get_str(c, t, "compress", false, NULL);
    if (comp) {
        if (strcmp(comp, "none") == 0) ir->compress = -1;
        else if (strncmp(comp, "mszip:", 6) == 0 && comp[6] >= '0' && comp[6] <= '9' && comp[7] == '\0') ir->compress = comp[6] - '0';
        else if (strcmp(comp, "mszip") != 0) ERR(c, ir_key_pos(t, "compress"), "RP1308", "a module's compress is \"none\", \"mszip\" or \"mszip:0\" ... \"mszip:9\"");
        rp_mem_free(c->alloc, comp);
    }
}

void ir_parse_feature(ctx_t *c, const rp_ttable_t *t, rp_ir_feature_t *f) {
    static const char *const keys[] = { "title", "description", "level", "hidden", "parent", "when", "required", "follow-parent", NULL };
    ir_check_keys(c, t, keys);
    ir_check_id(c, t, 38);
    f->id = ir_dup(c, t->id);
    f->pos = t->pos;
    f->title = ir_get_str(c, t, "title", true, NULL);
    f->description = ir_get_str(c, t, "description", false, NULL);
    f->level = (int32_t)ir_get_int(c, t, "level", 1, 1, 32767);
    f->hidden = ir_get_bool(c, t, "hidden", false);
    f->parent = ir_get_str(c, t, "parent", false, NULL);
    f->required = ir_get_bool(c, t, "required", false);
    f->follow_parent = ir_get_bool(c, t, "follow-parent", false);
    if (f->follow_parent && f->parent == NULL) ERR(c, ir_key_pos(t, "follow-parent"), "RP1316", "follow-parent needs a parent");
    f->when = ir_get_when(c, t);
}

void ir_parse_dir(ctx_t *c, const rp_ttable_t *t, rp_ir_dir_t *d) {
    static const char *const keys[] = { "path", "feature", "guard", NULL };
    ir_check_keys(c, t, keys);
    ir_check_id(c, t, 72);
    d->id = ir_dup(c, t->id);
    d->pos = t->pos;
    if (ir_windows_name(t->id)) ERR(c, t->pos, "RP1404", "[dir.%s]: '%s' is a Windows name; give the dir another ID", t->id, t->id);
    d->guard = ir_get_bool(c, t, "guard", false);
    d->feature = ir_get_str(c, t, "feature", false, NULL);
    char *path = ir_get_path(c, t, "path", true);
    if (path == NULL) return;
    // base/part/part..., or a known folder alone (the folder itself, e.g. "Fonts")
    size_t count = 1;
    for (const char *p = path; *p; ++p) count += *p == '/';
    if (count == 1 && ir_known_folder(path)) {
        d->base = path;
        d->parts = rp_mem_alloc(c->alloc, 1, sizeof *d->parts);     // non-NULL: a valid dir with no parts
        if (d->parts == NULL) c->nomem = true;
        return;
    }
    if (count < 2 || strchr(path, '\\')) {
        ERR(c, ir_key_pos(t, "path"), "RP1308", "path must be \"$(Base)/relative/path\" with '/' (Base is a folder such as ProgramFiles, or a dir ID)");
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
    char *base = ir_dup_n(c, save, (size_t)(slash - save));
    if (base && ir_known_folder(base)) d->base = base;
    else d->parent = base;
    save = slash + 1;
    for (size_t k = 0; k < count - 1; ++k) {
        char *next = strchr(save, '/');
        size_t n = next ? (size_t)(next - save) : strlen(save);
        d->parts[k] = ir_dup_n(c, save, n);
        if (d->parts[k]) ir_target_name_ok(c, d->parts[k], ir_key_pos(t, "path"));
        d->part_count++;
        save = next ? next + 1 : save + n;
    }
    rp_mem_free(c->alloc, path);
}

// RFC-0002 8.1: relative, '/' only.
bool ir_source_path_ok(ctx_t *c, const char *s, rp_pos_t pos) {
    if (strchr(s, '\\')) {
        ERR(c, pos, "RP1502", "source paths use '/', not '\\\\' (got '%s')", s);
        return false;
    }
    if (s[0] == '/' || (s[0] && s[1] == ':')) {
        ERR(c, pos, "RP1501", "source path '%s' must be relative to the source file", s);
        return false;
    }
    return true;
}

void ir_parse_file(ctx_t *c, const rp_ttable_t *t, rp_ir_file_t *f) {
    static const char *const keys[] = { "dir", "source", "name", "any-arch", "keep", "vital", "feature",
                                        "component-guid", "when", NULL };
    ir_check_keys(c, t, keys);
    ir_check_id(c, t, 72);
    f->id = ir_dup(c, t->id);
    f->pos = t->pos;
    f->when = ir_get_when(c, t);
    f->dir = ir_get_str(c, t, "dir", true, NULL);
    f->source = ir_get_str(c, t, "source", true, NULL);
    f->name = ir_get_str(c, t, "name", false, NULL);
    f->any_arch = ir_get_bool(c, t, "any-arch", false);
    f->keep = ir_get_bool(c, t, "keep", false);
    f->vital = ir_get_bool(c, t, "vital", true);
    f->msi_only = ir_get_bool(c, t, "msi-only", false);
    f->feature = ir_get_str(c, t, "feature", false, NULL);
    f->component_guid = ir_get_str(c, t, "component-guid", false, NULL);
    if (f->component_guid && !ir_guid_ok(f->component_guid)) {
        ERR(c, ir_key_pos(t, "component-guid"), "RP1308", "component-guid must be a GUID");
    }
    if (f->source == NULL) return;
    rp_pos_t sp = ir_key_pos(t, "source");
    const char *s = f->source;
    if (!ir_source_path_ok(c, s, sp)) return;
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
        f->name = ir_dup(c, b ? b + 1 : s);
    }
    if (f->name) ir_target_name_ok(c, f->name, ir_find_key(t, "name") ? ir_key_pos(t, "name") : sp);
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
    g->found[g->count++] = ir_dup(g->c, rel);
}

char *ir_join(ctx_t *c, const char *a, const char *b) {
    if (a == NULL || a[0] == '\0') return ir_dup(c, b);
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
    char *dir = rel[0] ? ir_join(c, g->root, rel) : ir_dup(c, g->root);
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
            char *child = ir_join(c, dir, names[k]);
            rp_fskind_t kind = child ? rp_pal_stat(c->alloc, child, NULL) : RP_FS_NONE;
            char *crel = ir_join(c, rel, names[k]);
            if (kind == RP_FS_DIR && crel) glob_walk(g, crel, seg, depth + 1);
            else if (kind == RP_FS_FILE && last && crel) glob_add(g, crel);
            rp_mem_free(c->alloc, child);
            rp_mem_free(c->alloc, crel);
        }
    } else {
        for (size_t k = 0; k < n; ++k) {
            if (!seg_match(pat, names[k])) continue;
            char *child = ir_join(c, dir, names[k]);
            rp_fskind_t kind = child ? rp_pal_stat(c->alloc, child, NULL) : RP_FS_NONE;
            char *crel = ir_join(c, rel, names[k]);
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

// Implicit sub folder `name` below dir `parent`, created once.
static const char *implicit_dir(ctx_t *c, const char *parent, const char *name, rp_pos_t pos) {
    char *logical = ir_join(c, parent, name);
    if (logical == NULL) return NULL;
    char key[23];
    rp_key_derive('D', logical, key);
    rp_mem_free(c->alloc, logical);
    const rp_ir_dir_t *have = ir_find_dir(c->ir, key);
    if (have) return have->id;
    rp_ir_dir_t *d = ir_push_dir(c);
    if (d == NULL) return NULL;
    d->id = ir_dup(c, key);
    d->parent = ir_dup(c, parent);
    d->parts = rp_mem_alloc(c->alloc, 1, sizeof *d->parts);
    if (d->parts) {
        d->parts[0] = ir_dup(c, name);
        d->part_count = 1;
    }
    d->implicit = true;
    d->pos = pos;
    ir_target_name_ok(c, name, pos);
    return d->id;
}

void ir_expand_files(ctx_t *c, const rp_ttable_t *t) {
    static const char *const keys[] = { "dir", "glob", "feature", "keep", "vital", "any-arch", "when", NULL };
    ir_check_keys(c, t, keys);
    ir_check_id(c, t, 72);
    char *dir = ir_get_str(c, t, "dir", true, NULL);
    char *pattern = ir_get_str(c, t, "glob", true, NULL);
    char *feature = ir_get_str(c, t, "feature", false, NULL);
    bool vital = ir_get_bool(c, t, "vital", true), any_arch = ir_get_bool(c, t, "any-arch", false);
    bool msi_only = ir_get_bool(c, t, "msi-only", false);
    bool keep = ir_get_bool(c, t, "keep", false);      // RFC-0013 A7
    char *when = ir_get_when(c, t);                    // RFC-0013 A2
    rp_pos_t gp = ir_key_pos(t, "glob");
    glob_t g = { .c = c, .pos = gp };
    if (dir && pattern && ir_source_path_ok(c, pattern, gp)) {
        const rp_ir_dir_t *d = ir_find_dir(c->ir, dir);
        if (d == NULL) ERR(c, ir_key_pos(t, "dir"), "RP1307", "dir '%s' is not defined", dir);
        if (feature == NULL && d && d->feature) feature = ir_dup(c, d->feature);
        // Split into segments; the leading ones without wildcards are the root.
        size_t nseg = 1;
        for (const char *q = pattern; *q; ++q) nseg += *q == '/';
        g.segs = rp_mem_alloc(c->alloc, nseg, sizeof *g.segs);
        char *copy = ir_dup(c, pattern);
        size_t lit = 0;
        bool wild = false;
        for (char *q = copy, *next; g.segs && copy && q; q = next) {
            next = strchr(q, '/');
            if (next) *next++ = '\0';
            g.segs[g.nseg] = ir_dup(c, q);
            bool has = strpbrk(q, "*?") != NULL;
            if (!wild && !has) ++lit;
            wild |= has;
            ++g.nseg;
        }
        if (lit == g.nseg) --lit;           // no wildcard: the last segment is the file itself
        char *root = ir_dup(c, c->opt->source_dir ? c->opt->source_dir : ".");
        for (size_t k = 0; k < lit && root; ++k) {
            char *r = ir_join(c, root, g.segs[k]);
            rp_mem_free(c->alloc, root);
            root = r;
        }
        g.root = root;
        // The source path as written, relative to the .rpk: the literal segments, then rel.
        char *prefix = ir_dup(c, "");
        for (size_t k = 0; k < lit && prefix; ++k) {
            char *r = ir_join(c, prefix, g.segs[k]);
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
            char *walk = ir_dup(c, rel);
            char *q = walk;
            for (char *slash; q && (slash = strchr(q, '/')) != NULL; q = slash + 1) {
                *slash = '\0';
                at = implicit_dir(c, at, q, gp);
                if (at == NULL) break;
            }
            char *logical = ir_join(c, t->id, rel);
            char key[23];
            if (logical) rp_key_derive('F', logical, key);
            rp_ir_file_t *f = ir_push_file(c);
            if (f && at && q && logical) {
                f->id = ir_dup(c, key);
                f->dir = ir_dup(c, at);
                f->source = ir_join(c, prefix, rel);
                f->source_path = ir_join(c, g.root, rel);
                f->name = ir_dup(c, q);
                f->vital = vital;
                f->keep = keep;
                f->when = ir_dup(c, when);
                f->msi_only = msi_only;
                f->any_arch = any_arch;
                f->feature = feature ? ir_dup(c, feature) : NULL;
                f->pos = gp;
                if (rp_pal_stat(c->alloc, f->source_path, &f->size) != RP_FS_FILE) {
                    ERR(c, gp, "RP1508", "'%s' is not a regular file", f->source);
                }
                if (c->opt->output && rp_pal_same_file(c->alloc, f->source_path, c->opt->output)) {
                    ERR(c, gp, "RP1505", "glob '%s' matches the output file '%s'", pattern, c->opt->output);
                }
                ir_target_name_ok(c, f->name, gp);
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
    rp_mem_free(c->alloc, when);
}

void ir_parse_folder(ctx_t *c, const rp_ttable_t *t, rp_ir_folder_t *f) {
    static const char *const keys[] = { "dir", "name", "keep", "feature", NULL };
    ir_check_keys(c, t, keys);
    ir_check_id(c, t, 72);
    f->id = ir_dup(c, t->id);
    f->pos = t->pos;
    f->dir = ir_get_str(c, t, "dir", true, NULL);
    f->name = ir_get_str(c, t, "name", true, NULL);
    f->keep = ir_get_bool(c, t, "keep", false);
    f->feature = ir_get_str(c, t, "feature", false, NULL);
    if (f->name) ir_target_name_ok(c, f->name, ir_key_pos(t, "name"));
}
