// src/model/ir_tables.c - the other item tables: [arp], [msix], properties, actions, registry,
// shortcuts and the rest.

#include "ir_int.h"

#include "rubrapack/num.h"

#include <stdio.h>

// Properties the tool writes itself, or that belong to the engine (RFC-0003 1).
bool ir_tool_property(const char *s) {
    static const char *const names[] = { "ALLUSERS", "REBOOT", "SECURECUSTOMPROPERTIES", "MSIHIDDENPROPERTIES",
                                         "INSTALLLEVEL", "REMOVE", "REINSTALL", "ADDLOCAL", "TARGETDIR", "PRODUCTCODE",
                                         "UPGRADECODE", NULL };
    for (size_t k = 0; names[k]; ++k) {
        if (strcmp(s, names[k]) == 0) return true;
    }
    return strncmp(s, "ARP", 3) == 0 || strncmp(s, "RP_", 3) == 0 || strncmp(s, "MSI", 3) == 0;
}

void ir_parse_arp(ctx_t *c, const rp_ttable_t *t) {
    static const char *const keys[] = { "no-modify", "no-repair", "help", "about", "icon", NULL };
    ir_check_keys(c, t, keys);
    c->ir->arp_no_modify = ir_get_bool(c, t, "no-modify", false);
    c->ir->arp_no_repair = ir_get_bool(c, t, "no-repair", false);
    c->ir->arp_help = ir_get_str(c, t, "help", false, NULL);
    c->ir->arp_about = ir_get_str(c, t, "about", false, NULL);
    c->ir->arp_icon_shown = ir_get_str(c, t, "icon", false, NULL);
    if (c->ir->arp_icon_shown) c->ir->arp_icon_source = ir_icon_source(c, c->ir->arp_icon_shown, ir_key_pos(t, "icon"));
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

void ir_parse_msix(ctx_t *c, const rp_ttable_t *t) {
    static const char *const keys[] = { "identity-name", "publisher", "publisher-display-name", "min-version", "appinstaller-uri",
                                        "package-uri", "update-hours", "update-prompt", "update-blocks", "update-background",
                                        "display-name", "capabilities", "file-system-virtualization", "registry-virtualization",
                                        "main-package", "main-publisher", "modification", "language-packs", NULL };
    static const char *const lang_bases[] = { "display-name", "publisher-display-name", NULL };
    ir_check_keys_lang(c, t, keys, lang_bases);
    rp_ir_t *ir = c->ir;
    ir->has_msix = true;
    ir->msix_display = ir_get_str(c, t, "display-name", false, NULL);
    ir_get_ltexts(c, t, "display-name", &ir->msix_display_by_lang, &ir->msix_display_by_lang_count);
    ir_get_ltexts(c, t, "publisher-display-name", &ir->msix_publisher_display_by_lang, &ir->msix_publisher_display_by_lang_count);
    ir->msix_pos = t->pos;
    ir->msix_identity_name = ir_get_str(c, t, "identity-name", true, NULL);
    ir->msix_publisher = ir_get_str(c, t, "publisher", true, NULL);
    ir->msix_publisher_display = ir_get_str(c, t, "publisher-display-name", false, NULL);
    ir->msix_min_version = ir_get_str(c, t, "min-version", false, NULL);
    if (ir->msix_identity_name && !msix_name_ok(ir->msix_identity_name)) {
        ERR(c, ir_key_pos(t, "identity-name"), "RP1601", "identity-name must be 3 to 50 characters of A-Z, a-z, 0-9, '.' and '-' (got '%s')",
            ir->msix_identity_name);
    }
    if (ir->msix_publisher && (strchr(ir->msix_publisher, '=') == NULL || strlen(ir->msix_publisher) > 8192)) {
        ERR(c, ir_key_pos(t, "publisher"), "RP1602", "publisher must be the signing certificate's subject, such as \"CN=Example, O=Example, C=KR\"");
    }
    if (ir->msix_publisher && strstr(ir->msix_publisher, "OID.2.25.311729368913984317654407730594956997722")) {
        ERR(c, ir_key_pos(t, "publisher"), "RP1602", "publisher must not carry the unsigned-test OID; --unsigned-test adds it");
    }
    if (ir->msix_min_version && !four_part_version(ir->msix_min_version)) {
        ERR(c, ir_key_pos(t, "min-version"), "RP1603", "min-version must have four parts, such as 10.0.17763.0");
    }
    // RFC-0018: capabilities beyond runFullTrust (checked by name when the MSIX is built), and
    // write virtualization turned off.
    const rp_tkey_t *caps = ir_find_key(t, "capabilities");
    if (caps) {
        ir->msix_caps_pos = caps->pos;
        if (caps->val.kind != RP_TV_ARRAY || caps->val.items[0].kind != RP_TV_STRING) {
            ERR(c, caps->pos, "RP1306", "capabilities is an array of names, like [\"internetClient\", \"allowElevation\"]");
        } else {
            ir->msix_caps = rp_mem_alloc(c->alloc, caps->val.count, sizeof *ir->msix_caps);
            if (ir->msix_caps == NULL) c->nomem = true;
            for (size_t k = 0; ir->msix_caps && k < caps->val.count; ++k) {
                char *s = ir_subst(c, &caps->val.items[k]);
                if (s) ir->msix_caps[ir->msix_cap_count++] = s;
            }
        }
    }
    // RFC-0019: an optional package of another (main-package), or a modification package of it.
    ir->msix_main = ir_get_str(c, t, "main-package", false, NULL);
    ir->msix_main_publisher = ir_get_str(c, t, "main-publisher", false, NULL);
    ir->msix_modification = ir_get_bool(c, t, "modification", false);
    ir->msix_no_langpacks = !ir_get_bool(c, t, "language-packs", true);
    if (ir->msix_main && !msix_name_ok(ir->msix_main)) {
        ERR(c, ir_key_pos(t, "main-package"), "RP1601", "main-package is the main package's identity name (got '%s')", ir->msix_main);
    }
    if (ir->msix_main_publisher && strchr(ir->msix_main_publisher, '=') == NULL) {
        ERR(c, ir_key_pos(t, "main-publisher"), "RP1602", "main-publisher is the main package's publisher, such as \"CN=Example\"");
    }
    if ((ir->msix_main_publisher || ir->msix_modification) && ir->msix_main == NULL) {
        ERR(c, t->pos, "RP1617", "main-publisher and modification = true go with main-package (the package this one belongs to)");
    }
    ir->msix_no_fs_virt = !ir_get_bool(c, t, "file-system-virtualization", true);
    ir->msix_no_reg_virt = !ir_get_bool(c, t, "registry-virtualization", true);
    // RFC-0016 3: where the .appinstaller file and the package will be downloaded from.
    ir->msix_appinstaller_uri = ir_get_str(c, t, "appinstaller-uri", false, NULL);
    ir->msix_package_uri = ir_get_str(c, t, "package-uri", false, NULL);
    ir->msix_update_hours = (int)ir_get_int(c, t, "update-hours", 24, 0, 255);
    ir->msix_update_prompt = ir_get_bool(c, t, "update-prompt", false);
    ir->msix_update_blocks = ir_get_bool(c, t, "update-blocks", false);
    ir->msix_update_background = ir_get_bool(c, t, "update-background", false);
    if ((ir->msix_appinstaller_uri == NULL) != (ir->msix_package_uri == NULL)) {
        ERR(c, t->pos, "RP1615", "appinstaller-uri and package-uri go together: where the .appinstaller file and the package will be");
    }
    const char *uris[] = { "appinstaller-uri", "package-uri" };
    const char *vals[] = { ir->msix_appinstaller_uri, ir->msix_package_uri };
    for (int k = 0; k < 2; ++k) {
        const char *u = vals[k];
        if (u && strncmp(u, "https://", 8) != 0 && strncmp(u, "http://", 7) != 0 && strncmp(u, "file://", 7) != 0 && strncmp(u, "\\\\", 2) != 0) {
            ERR(c, ir_key_pos(t, uris[k]), "RP1615", "%s must be an https://, http:// or file:// address, or a \\\\server\\share path (got '%s')", uris[k], u);
        }
    }
    if (ir->msix_appinstaller_uri == NULL && (ir_find_key(t, "update-hours") || ir_find_key(t, "update-prompt") || ir_find_key(t, "update-blocks") ||
                                              ir_find_key(t, "update-background"))) {
        ERR(c, t->pos, "RP1615", "update-* keys need appinstaller-uri and package-uri");
    }
}

char *ir_logo_path(ctx_t *c, const rp_ttable_t *t, const char *key, char **shown) {
    *shown = ir_get_str(c, t, key, false, NULL);
    if (*shown == NULL) return NULL;
    rp_pos_t p = ir_key_pos(t, key);
    if (!ir_source_path_ok(c, *shown, p)) return NULL;
    char *path = ir_join(c, c->opt->source_dir ? c->opt->source_dir : ".", *shown);
    uint64_t size;
    if (path && rp_pal_stat(c->alloc, path, &size) != RP_FS_FILE) {
        // RFC-0016 3: scale variants alone (Logo.scale-100.png ...) stand in for Logo.png.
        bool variant = false;
        const char *dot = strrchr(path, '.'), *slash = strrchr(path, '/');
        static const char *const scales[] = { "100", "125", "150", "200", "400" };
        for (size_t k = 0; dot && (!slash || dot > slash) && !variant && k < 5; ++k) {
            char v[2048];
            snprintf(v, sizeof v, "%.*s.scale-%s%s", (int)(dot - path), path, scales[k], dot);
            variant = rp_pal_stat(c->alloc, v, &size) == RP_FS_FILE;
        }
        if (!variant) ERR(c, p, "RP1507", "logo '%s' not found (nor as .scale-100 ... .scale-400 variants)", *shown);
    }
    return path;
}

void ir_parse_msix_app(ctx_t *c, const rp_ttable_t *t) {
    static const char *const keys[] = { "executable", "display-name", "description", "logo-150", "logo-44", "store-logo",
                                        "background-color", "hidden", NULL };
    static const char *const lang_bases[] = { "display-name", "description", NULL };
    ir_check_keys_lang(c, t, keys, lang_bases);
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
    a->id = ir_dup(c, t->id);
    a->pos = t->pos;
    a->exe = ir_get_str(c, t, "executable", true, NULL);
    a->display = ir_get_str(c, t, "display-name", false, NULL);
    a->description = ir_get_str(c, t, "description", false, NULL);
    ir_get_ltexts(c, t, "display-name", &a->display_by_lang, &a->display_by_lang_count);
    ir_get_ltexts(c, t, "description", &a->description_by_lang, &a->description_by_lang_count);
    static const char *const logos[3] = { "logo-150", "logo-44", "store-logo" };
    int given = 0;
    for (int i = 0; i < 3; ++i) {
        a->logo_path[i] = ir_logo_path(c, t, logos[i], &a->logo[i]);
        given += a->logo[i] != NULL;
    }
    if (given != 0 && given != 3) ERR(c, t->pos, "RP1608", "give all three logos (logo-150, logo-44, store-logo) or none");
    // RFC-0018: the tile's background and whether the application shows in the Start menu.
    a->background = ir_get_str(c, t, "background-color", false, NULL);
    if (a->background && strcmp(a->background, "transparent") != 0) {
        bool ok = strlen(a->background) == 7 && a->background[0] == '#';
        for (int k = 1; ok && k < 7; ++k) ok = ir_is_hex(a->background[k]);
        if (!ok) ERR(c, ir_key_pos(t, "background-color"), "RP1308", "background-color is \"transparent\" or \"#RRGGBB\", like \"#1E3A5F\"");
    }
    a->hidden = ir_get_bool(c, t, "hidden", false);
    bool id_ok = t->id[0] != '\0' && strlen(t->id) <= 64 && !(t->id[0] >= '0' && t->id[0] <= '9');
    for (const char *p = t->id; *p; ++p) id_ok &= (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9');
    if (!id_ok) ERR(c, t->pos, "RP1606", "the application ID '%s' becomes Application Id: letters and digits only, not starting with a digit", t->id);
}

// [msix-dependency.ID] (RFC-0018): a framework package, such as Microsoft.VCLibs.140.00.UWPDesktop.
void ir_parse_msix_dep(ctx_t *c, const rp_ttable_t *t) {
    static const char *const keys[] = { "name", "publisher", "min-version", NULL };
    ir_check_keys(c, t, keys);
    ir_check_id(c, t, 72);
    rp_ir_t *ir = c->ir;
    rp_ir_msix_dep_t *nd = rp_mem_alloc(c->alloc, ir->msix_dep_count + 1, sizeof *nd);
    if (nd == NULL) {
        c->nomem = true;
        return;
    }
    if (ir->msix_dep_count) memcpy(nd, ir->msix_deps, ir->msix_dep_count * sizeof *nd);
    rp_mem_free(c->alloc, ir->msix_deps);
    ir->msix_deps = nd;
    rp_ir_msix_dep_t *d = &nd[ir->msix_dep_count++];
    memset(d, 0, sizeof *d);
    d->id = ir_dup(c, t->id);
    d->pos = t->pos;
    d->name = ir_get_str(c, t, "name", true, NULL);
    d->publisher = ir_get_str(c, t, "publisher", true, NULL);
    d->min_version = ir_get_str(c, t, "min-version", true, NULL);
    if (d->name && !msix_name_ok(d->name)) ERR(c, ir_key_pos(t, "name"), "RP1601", "name is a package's identity name (got '%s')", d->name);
    if (d->publisher && strchr(d->publisher, '=') == NULL) {
        ERR(c, ir_key_pos(t, "publisher"), "RP1602", "publisher is the package's publisher, such as \"CN=Microsoft Corporation, O=Microsoft Corporation, L=Redmond, S=Washington, C=US\"");
    }
    if (d->min_version && !four_part_version(d->min_version)) ERR(c, ir_key_pos(t, "min-version"), "RP1603", "min-version must have four parts, such as 14.0.24217.0");
}

void ir_parse_property(ctx_t *c, const rp_ttable_t *t, rp_ir_property_t *p) {
    static const char *const keys[] = { "value", "secure", "hidden", NULL };
    ir_check_keys(c, t, keys);
    p->id = ir_dup(c, t->id);
    p->pos = t->pos;
    bool upper = t->id[0] != '\0' && strlen(t->id) <= 72;
    for (const char *s = t->id; *s; ++s) upper &= (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || *s == '_';
    if (!upper || (t->id[0] >= '0' && t->id[0] <= '9')) {
        ERR(c, t->pos, "RP1310", "property '%s' must be a public name: upper-case letters, digits and '_'", t->id);
    } else if (ir_tool_property(t->id)) {
        ERR(c, t->pos, "RP1310", "property '%s' is set by rubrapack or the installer and cannot be defined here", t->id);
    }
    p->value = ir_get_str(c, t, "value", true, NULL);
    p->secure = ir_get_bool(c, t, "secure", false);
    p->hidden = ir_get_bool(c, t, "hidden", false);
}

void ir_parse_action(ctx_t *c, const rp_ttable_t *t, rp_ir_action_t *a) {
    static const char *const keys[] = { "run", "do", "undo", "check", NULL };
    ir_check_keys(c, t, keys);
    ir_check_id(c, t, 40);            // leaves room for the RP_<ID>_<Suffix> keys (72)
    a->id = ir_dup(c, t->id);
    a->pos = t->pos;
    char *run = ir_get_str(c, t, "run", true, NULL);
    if (run) {
        if (strncmp(run, "file:", 5) != 0 || run[5] == '\0') {
            ERR(c, ir_key_pos(t, "run"), "RP1311", "run must be \"file:<ID>\" naming a [file.*] of this package");
        } else {
            a->run_file = ir_dup(c, run + 5);
        }
        rp_mem_free(c->alloc, run);
    }
    bool has_do = false, has_undo = false;
    a->do_args = ir_get_fmt(c, t, "do", false, &has_do, IR_FMT_INSTALL);
    a->undo_args = ir_get_fmt(c, t, "undo", false, &has_undo, IR_FMT_INSTALL);
    a->check_args = ir_get_fmt(c, t, "check", false, NULL, IR_FMT_INSTALL);
    if (a->check_args) {
        rp_srcdiag_add(c->d, ir_key_pos(t, "check"), "RP1318", true,
                       "[action.%s] check has no effect: nothing runs it yet; remove it", t->id);
    }
    if (!has_do || !has_undo) {
        ERR(c, t->pos, "RP1312", "[action.%s] needs both do and undo (the undo also rolls back a failed do)", t->id);
    }
}

void ir_parse_registry(ctx_t *c, const rp_ttable_t *t, rp_ir_registry_t *r) {
    static const char *const keys[] = { "root", "key", "name", "value", "type", "remove", "keep", "view", "with",
                                        "feature", "when", NULL };
    ir_check_keys(c, t, keys);
    ir_check_id(c, t, 72);
    r->id = ir_dup(c, t->id);
    r->pos = t->pos;
    r->when = ir_get_when(c, t);
    if (r->when && ir_find_key(t, "with")) ERR(c, ir_key_pos(t, "when"), "RP1316", "a value `with` a file shares its component: put `when` on the file");
    r->msi_only = ir_get_bool(c, t, "msi-only", false);
    char *root = ir_get_str(c, t, "root", true, NULL);
    if (root) {
        // Per scope: machine HKLM/HKCR/HKMU, user HKCU/HKCR/HKMU, dual HKMU/HKCR (HKMU = HKLM for a
        // per-machine installation, HKCU for a per-user one).
        int scope = c->ir->scope;
        if (strcmp(root, "HKLM") == 0) r->root = RP_ROOT_HKLM;
        else if (strcmp(root, "HKCR") == 0) r->root = RP_ROOT_HKCR;
        else if (strcmp(root, "HKCU") == 0) r->root = RP_ROOT_HKCU;
        else if (strcmp(root, "HKMU") == 0) r->root = RP_ROOT_HKMU;
        else ERR(c, ir_key_pos(t, "root"), "RP1316", "root must be HKLM, HKCU, HKCR or HKMU (got '%s')", root);
        if (r->root == RP_ROOT_HKCU && scope == 0) {
            ERR(c, ir_key_pos(t, "root"), "RP1316", "a per-machine package does not write HKCU (it would be the installing user's); use scope = \"user\"");
        } else if (r->root == RP_ROOT_HKLM && scope != 0) {
            ERR(c, ir_key_pos(t, "root"), "RP1316", "a %s package cannot write HKLM; use HKMU", scope == 1 ? "per-user" : "dual");
        } else if (r->root == RP_ROOT_HKCU && scope == 2) {
            ERR(c, ir_key_pos(t, "root"), "RP1316", "a dual package writes HKMU (HKLM or HKCU as installed), not HKCU");
        }
        rp_mem_free(c->alloc, root);
    }
    r->key = ir_get_str(c, t, "key", true, NULL);
    if (r->key && (r->key[0] == '\\' || r->key[strlen(r->key) - 1] == '\\' || strstr(r->key, "\\\\") || ir_has_control(r->key))) {
        ERR(c, ir_key_pos(t, "key"), "RP1316", "key '%s' must not start or end with '\\' or contain an empty part", r->key);
    }
    r->name = ir_get_str(c, t, "name", false, NULL);
    if (r->name && r->name[0] == '\0') {        // "" is the default value, as omitting it
        rp_mem_free(c->alloc, r->name);
        r->name = NULL;
    }
    r->remove = ir_get_bool(c, t, "remove", false);
    r->keep = ir_get_bool(c, t, "keep", false);
    char *view = ir_get_str(c, t, "view", false, NULL);
    if (view) {
        if (strcmp(view, "32") == 0 && c->ir->arch != RP_ARCH_X86) r->view32 = true;
        else if (strcmp(view, "32") != 0 && strcmp(view, "64") != 0) {
            ERR(c, ir_key_pos(t, "view"), "RP1316", "view must be \"32\" or \"64\"");
        } else if (strcmp(view, "64") == 0 && c->ir->arch == RP_ARCH_X86) {
            ERR(c, ir_key_pos(t, "view"), "RP1316", "an x86 package writes the 32-bit registry view only");
        }
        rp_mem_free(c->alloc, view);
    }
    char *with = ir_get_str(c, t, "with", false, NULL);
    if (with) {
        if (strncmp(with, "file:", 5) != 0 || with[5] == '\0') ERR(c, ir_key_pos(t, "with"), "RP1315", "with must be \"file:<ID>\"");
        else r->with_file = ir_dup(c, with + 5);
        rp_mem_free(c->alloc, with);
    }
    r->feature = ir_get_str(c, t, "feature", false, NULL);
    if (r->with_file && r->feature) ERR(c, ir_key_pos(t, "feature"), "RP1316", "a value that goes 'with' a file takes that file's feature");

    char *type = ir_get_str(c, t, "type", false, NULL);
    static const char *const types[] = { "string", "expand", "dword", "binary", "multi", "qword" };
    r->type = RP_REG_STRING;
    if (type) {
        bool known = false;
        for (int k = 0; k < 6; ++k) {
            if (strcmp(type, types[k]) == 0) r->type = (rp_reg_type_t)k, known = true;
        }
        if (!known) ERR(c, ir_key_pos(t, "type"), "RP1316", "type must be string, expand, dword, qword, binary or multi (got '%s')", type);
        rp_mem_free(c->alloc, type);
    }
    const rp_tkey_t *v = ir_find_key(t, "value");
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
            for (size_t k = 2; k < v->val.len; ++k) ok &= ir_is_hex(v->val.str[k]);
            if (!ok) {
                ERR(c, v->pos, "RP1316", "a qword value is an integer or \"0x\" and 1 to 16 hex digits");
                return;
            }
            uint64_t q = 0;
            (void)rp_read_hex64(v->val.str + 2, NULL, &q);      // checked above: 1 to 16 hex digits
            snprintf(num, sizeof num, "%016llX", (unsigned long long)q);
        } else {
            ERR(c, v->pos, "RP1316", "a qword value is an integer or \"0x\" and 1 to 16 hex digits");
            return;
        }
        r->value = ir_dup(c, num);
        return;
    case RP_REG_DWORD:
        if (v->val.kind != RP_TV_INT || v->val.i < 0 || v->val.i > 0xFFFFFFFFll) {
            ERR(c, v->pos, "RP1316", "a dword value is an integer 0..4294967295 (0x0..0xFFFFFFFF)");
            return;
        }
        snprintf(num, sizeof num, "%lld", (long long)v->val.i);
        r->value = ir_dup(c, num);
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
            c->fmt = IR_FMT_INSTALL;
            r->items[r->item_count++] = ir_subst(c, &v->val.items[k]);
            c->fmt = IR_FMT_NONE;
        }
        return;
    default:
        break;
    }
    r->value = ir_get_fmt(c, t, "value", false, NULL, r->type == RP_REG_EXPAND ? IR_FMT_RUNTIME : IR_FMT_INSTALL);
    if (r->value && (r->type == RP_REG_STRING || r->type == RP_REG_EXPAND) && strstr(r->value, "[~]")) {
        ERR(c, v->pos, "RP1316", "'[~]' makes Windows Installer write a multi-string; use type = \"multi\" and an array");
    }
    if (r->value && r->type == RP_REG_BINARY) {
        size_t n = strlen(r->value);
        bool ok = n > 0 && n % 2 == 0;
        for (size_t k = 0; ok && k < n; ++k) ok = ir_is_hex(r->value[k]);
        if (!ok) ERR(c, v->pos, "RP1316", "a binary value is an even number of hex digits, like \"01A0FF\"");
        for (size_t k = 0; ok && k < n; ++k) {
            if (r->value[k] >= 'a' && r->value[k] <= 'f') r->value[k] = (char)(r->value[k] - 32);
        }
    }
}

bool ir_shortcut_folder(const char *s) {
    return strcmp(s, "Programs") == 0 || strcmp(s, "Desktop") == 0 || strcmp(s, "StartMenu") == 0 || strcmp(s, "Startup") == 0;
}

void ir_parse_shortcut(ctx_t *c, const rp_ttable_t *t, rp_ir_shortcut_t *s) {
    static const char *const keys[] = { "dir", "name", "target", "args", "description", "working-dir", "icon", "when", NULL };
    ir_check_keys(c, t, keys);
    ir_check_id(c, t, 72);
    s->id = ir_dup(c, t->id);
    s->pos = t->pos;
    s->when = ir_get_when(c, t);
    s->icon_shown = ir_get_str(c, t, "icon", false, NULL);
    if (s->icon_shown) s->icon_source = ir_icon_source(c, s->icon_shown, ir_key_pos(t, "icon"));
    s->dir = ir_get_str(c, t, "dir", true, NULL);
    s->name = ir_get_str(c, t, "name", true, NULL);
    if (s->name) ir_target_name_ok(c, s->name, ir_key_pos(t, "name"));
    char *target = ir_get_str(c, t, "target", true, NULL);
    if (target) {
        if (strncmp(target, "file:", 5) != 0 || target[5] == '\0') {
            ERR(c, ir_key_pos(t, "target"), "RP1315", "target must be \"file:<ID>\" naming a [file.*] of this package");
        } else {
            s->target_file = ir_dup(c, target + 5);
        }
        rp_mem_free(c->alloc, target);
    }
    s->args = ir_get_fmt(c, t, "args", false, NULL, IR_FMT_INSTALL);
    s->description = ir_get_str(c, t, "description", false, NULL);
    s->working_dir = ir_get_str(c, t, "working-dir", false, NULL);
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
    bool ok = ir_target_name_ok(c, probe, pos);
    rp_mem_free(c->alloc, probe);
    return ok;
}

void ir_parse_remove(ctx_t *c, const rp_ttable_t *t, rp_ir_remove_t *r) {
    static const char *const keys[] = { "dir", "name", "on", "feature", NULL };
    ir_check_keys(c, t, keys);
    ir_check_id(c, t, 72);
    r->id = ir_dup(c, t->id);
    r->pos = t->pos;
    r->dir = ir_get_str(c, t, "dir", true, NULL);
    r->name = ir_get_str(c, t, "name", false, NULL);
    if (r->name) pattern_ok(c, r->name, ir_key_pos(t, "name"));
    char *on = ir_get_str(c, t, "on", true, NULL);
    if (on) {
        if (strcmp(on, "install") == 0) r->mode = 1;
        else if (strcmp(on, "uninstall") == 0) r->mode = 2;
        else if (strcmp(on, "both") == 0) r->mode = 3;
        else ERR(c, ir_key_pos(t, "on"), "RP1316", "on must be \"install\", \"uninstall\" or \"both\"");
        rp_mem_free(c->alloc, on);
    }
    r->feature = ir_get_str(c, t, "feature", false, NULL);
}

void ir_parse_copy(ctx_t *c, const rp_ttable_t *t, rp_ir_copy_t *cp) {
    static const char *const keys[] = { "source", "dir", "name", NULL };
    ir_check_keys(c, t, keys);
    ir_check_id(c, t, 72);
    cp->id = ir_dup(c, t->id);
    cp->pos = t->pos;
    cp->msi_only = ir_get_bool(c, t, "msi-only", false);
    char *src = ir_get_str(c, t, "source", true, NULL);
    if (src) {
        if (strncmp(src, "file:", 5) != 0 || src[5] == '\0') ERR(c, ir_key_pos(t, "source"), "RP1315", "source must be \"file:<ID>\"");
        else cp->source_file = ir_dup(c, src + 5);
        rp_mem_free(c->alloc, src);
    }
    cp->dir = ir_get_str(c, t, "dir", true, NULL);
    cp->name = ir_get_str(c, t, "name", false, NULL);
    if (cp->name) ir_target_name_ok(c, cp->name, ir_key_pos(t, "name"));
}

void ir_parse_ini(ctx_t *c, const rp_ttable_t *t, rp_ir_ini_t *x) {
    static const char *const keys[] = { "dir", "file", "section", "key", "value", "mode", "feature", "when", NULL };
    ir_check_keys(c, t, keys);
    ir_check_id(c, t, 72);
    x->id = ir_dup(c, t->id);
    x->pos = t->pos;
    x->msi_only = ir_get_bool(c, t, "msi-only", false);
    x->when = ir_get_when(c, t);
    x->dir = ir_get_str(c, t, "dir", true, NULL);
    x->file = ir_get_str(c, t, "file", true, NULL);
    if (x->file) ir_target_name_ok(c, x->file, ir_key_pos(t, "file"));
    x->section = ir_get_str(c, t, "section", true, NULL);
    x->key = ir_get_str(c, t, "key", true, NULL);
    if ((x->section && (strchr(x->section, ']') || ir_has_control(x->section))) || (x->key && (strchr(x->key, '=') || ir_has_control(x->key)))) {
        ERR(c, t->pos, "RP1316", "an INI section may not contain ']' and a key may not contain '='");
    }
    char *mode = ir_get_str(c, t, "mode", false, NULL);
    if (mode) {
        if (strcmp(mode, "set") == 0) x->mode = 0;
        else if (strcmp(mode, "add") == 0) x->mode = 1;
        else if (strcmp(mode, "remove") == 0) x->mode = 2;
        else ERR(c, ir_key_pos(t, "mode"), "RP1316", "mode must be \"set\", \"add\" or \"remove\"");
        rp_mem_free(c->alloc, mode);
    }
    bool has_value = false;
    x->value = ir_get_fmt(c, t, "value", false, &has_value, IR_FMT_INSTALL);
    if (x->mode != 2 && !has_value) ERR(c, t->pos, "RP1202", "[ini.%s] needs 'value'", t->id);
    if (x->mode == 2 && has_value) ERR(c, ir_key_pos(t, "value"), "RP1316", "mode = \"remove\" removes the key; it takes no 'value'");
    x->feature = ir_get_str(c, t, "feature", false, NULL);
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

char *ir_get_when(ctx_t *c, const rp_ttable_t *t) {
    char *w = ir_get_str(c, t, "when", false, NULL);
    if (w == NULL) return NULL;
    if (w[0] == '\0' || !condition_ok(w)) ERR(c, ir_key_pos(t, "when"), "RP1316", "when is an MSI condition with closed quotes and balanced parentheses");
    else if (strlen(w) > 240) ERR(c, ir_key_pos(t, "when"), "RP1316", "when is longer than 240 characters");
    return w;
}

void ir_parse_require(ctx_t *c, const rp_ttable_t *t, rp_ir_require_t *r) {
    static const char *const keys[] = { "condition", "message", NULL };
    ir_check_keys(c, t, keys);
    ir_check_id(c, t, 72);
    r->id = ir_dup(c, t->id);
    r->pos = t->pos;
    r->condition = ir_get_str(c, t, "condition", true, NULL);
    if (r->condition && !condition_ok(r->condition)) {
        ERR(c, ir_key_pos(t, "condition"), "RP1316", "condition has an unclosed quote or unbalanced parentheses");
    }
    if (r->condition && strlen(r->condition) > 240) ERR(c, ir_key_pos(t, "condition"), "RP1316", "condition is longer than 240 characters (rubrapack adds \"Installed OR ( )\")");
    r->message = ir_get_fmt(c, t, "message", true, NULL, IR_FMT_INSTALL);
}

void ir_parse_search(ctx_t *c, const rp_ttable_t *t, rp_ir_search_t *x) {
    static const char *const keys[] = { "property", "kind", "root", "key", "name", "view", "path", "file", "min-version",
                                        "component-guid", NULL };
    ir_check_keys(c, t, keys);
    ir_check_id(c, t, 72);
    x->id = ir_dup(c, t->id);
    x->pos = t->pos;
    x->property = ir_get_str(c, t, "property", true, NULL);
    if (x->property) {
        bool upper = true;
        for (const char *p = x->property; *p; ++p) upper &= (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_';
        if (!upper || ir_tool_property(x->property)) {
            ERR(c, ir_key_pos(t, "property"), "RP1310", "search property '%s' must be a public name of your own (upper case)", x->property);
        }
    }
    char *kind = ir_get_str(c, t, "kind", true, NULL);
    if (kind == NULL) return;
    if (strcmp(kind, "registry") == 0) x->kind = RP_SEARCH_REGISTRY;
    else if (strcmp(kind, "file") == 0) x->kind = RP_SEARCH_FILE;
    else if (strcmp(kind, "dir") == 0) x->kind = RP_SEARCH_DIR;
    else if (strcmp(kind, "component") == 0) x->kind = RP_SEARCH_COMPONENT;
    else ERR(c, ir_key_pos(t, "kind"), "RP1316", "kind must be registry, file, dir or component");
    rp_mem_free(c->alloc, kind);
    if (x->kind == RP_SEARCH_REGISTRY) {
        char *root = ir_get_str(c, t, "root", true, NULL);
        if (root) {
            if (strcmp(root, "HKLM") == 0) x->root = RP_ROOT_HKLM;
            else if (strcmp(root, "HKCR") == 0) x->root = RP_ROOT_HKCR;
            else if (strcmp(root, "HKCU") == 0) x->root = RP_ROOT_HKCU;
            else ERR(c, ir_key_pos(t, "root"), "RP1316", "root must be HKLM, HKCR or HKCU");
            rp_mem_free(c->alloc, root);
        }
        x->key = ir_get_str(c, t, "key", true, NULL);
        x->name = ir_get_str(c, t, "name", false, NULL);
        char *view = ir_get_str(c, t, "view", false, NULL);
        if (view) {
            if (strcmp(view, "32") == 0) x->view32 = true;
            else if (strcmp(view, "64") != 0) ERR(c, ir_key_pos(t, "view"), "RP1316", "view must be \"32\" or \"64\"");
            rp_mem_free(c->alloc, view);
        }
        if (c->ir->arch == RP_ARCH_X86) x->view32 = true;
    } else if (x->kind == RP_SEARCH_COMPONENT) {
        x->component_guid = ir_get_str(c, t, "component-guid", true, NULL);
        if (x->component_guid && !ir_guid_ok(x->component_guid)) ERR(c, ir_key_pos(t, "component-guid"), "RP1308", "component-guid must be a GUID");
    } else {
        char *path = ir_get_path(c, t, "path", true);         // $(Base) or $(Base)/rel/path, like [dir.*]
        if (path) {
            char *slash = strchr(path, '/');
            x->base = slash ? ir_dup_n(c, path, (size_t)(slash - path)) : ir_dup(c, path);
            if (x->base && !ir_known_folder(x->base)) ERR(c, ir_key_pos(t, "path"), "RP1316", "path must start with a Windows folder (like $(ProgramFiles) or $(System))");
            if (slash && slash[1]) {
                x->path = ir_dup(c, slash + 1);
                for (char *p = x->path; p && *p; ++p) {
                    if (*p == '/') *p = '\\';
                }
            }
            if (strchr(path, '\\')) ERR(c, ir_key_pos(t, "path"), "RP1502", "use '/' in path");
            rp_mem_free(c->alloc, path);
        }
        if (x->kind == RP_SEARCH_FILE) {
            x->file_name = ir_get_str(c, t, "file", true, NULL);
            if (x->file_name) ir_target_name_ok(c, x->file_name, ir_key_pos(t, "file"));
            x->min_version = ir_get_str(c, t, "min-version", false, NULL);
            uint16_t parts[4];
            size_t n = 0;
            if (x->min_version && !ir_parse_version(x->min_version, parts, &n)) {
                ERR(c, ir_key_pos(t, "min-version"), "RP1308", "min-version must be a version like 1.2.3");
            }
        }
    }
}

int ir_ascii_casecmp(const char *a, const char *b) {
    for (;; ++a, ++b) {
        char x = (char)(*a >= 'A' && *a <= 'Z' ? *a + 32 : *a), y = (char)(*b >= 'A' && *b <= 'Z' ? *b + 32 : *b);
        if (x != y || x == 0) return x - y;
    }
}

void ir_parse_service(ctx_t *c, const rp_ttable_t *t, rp_ir_service_t *x) {
    static const char *const keys[] = { "file", "name", "display-name", "description", "start", "account", "args",
                                        "start-on-install", NULL };
    ir_check_keys(c, t, keys);
    ir_check_id(c, t, 72);
    x->id = ir_dup(c, t->id);
    x->pos = t->pos;
    char *file = ir_get_str(c, t, "file", true, NULL);
    if (file) {
        if (strncmp(file, "file:", 5) != 0 || file[5] == '\0') ERR(c, ir_key_pos(t, "file"), "RP1315", "file must be \"file:<ID>\"");
        else x->file = ir_dup(c, file + 5);
        rp_mem_free(c->alloc, file);
    }
    x->name = ir_get_str(c, t, "name", true, NULL);
    if (x->name && (strpbrk(x->name, "/\\") || ir_has_control(x->name) || strlen(x->name) > 256)) {
        ERR(c, ir_key_pos(t, "name"), "RP1316", "a service name may not contain '/' or '\\' (at most 256 characters)");
    }
    x->display_name = ir_get_str(c, t, "display-name", false, NULL);
    x->description = ir_get_str(c, t, "description", false, NULL);
    x->args = ir_get_fmt(c, t, "args", false, NULL, IR_FMT_INSTALL);
    x->start = 3;
    char *start = ir_get_str(c, t, "start", false, NULL);
    if (start) {
        if (strcmp(start, "auto") == 0) x->start = 2;
        else if (strcmp(start, "demand") == 0) x->start = 3;
        else if (strcmp(start, "disabled") == 0) x->start = 4;
        else ERR(c, ir_key_pos(t, "start"), "RP1316", "start must be auto, demand or disabled");
        rp_mem_free(c->alloc, start);
    }
    char *account = ir_get_str(c, t, "account", false, NULL);
    if (account) {
        if (strcmp(account, "LocalSystem") == 0) x->account = 0;
        else if (strcmp(account, "LocalService") == 0) x->account = 1;
        else if (strcmp(account, "NetworkService") == 0) x->account = 2;
        else ERR(c, ir_key_pos(t, "account"), "RP1316", "account must be LocalSystem, LocalService or NetworkService");
        rp_mem_free(c->alloc, account);
    }
    x->start_on_install = ir_get_bool(c, t, "start-on-install", false);
    if (x->start_on_install && x->start == 4) ERR(c, ir_key_pos(t, "start-on-install"), "RP1316", "a disabled service cannot be started");
}

void ir_parse_permission(ctx_t *c, const rp_ttable_t *t, rp_ir_permission_t *x) {
    static const char *const keys[] = { "target", "sddl", NULL };
    ir_check_keys(c, t, keys);
    ir_check_id(c, t, 72);
    x->id = ir_dup(c, t->id);
    x->pos = t->pos;
    char *target = ir_get_str(c, t, "target", true, NULL);
    if (target) {
        static const char *const prefixes[] = { "dir:", "file:", "registry:" };
        x->kind = -1;
        for (int k = 0; k < 3; ++k) {
            size_t n = strlen(prefixes[k]);
            if (strncmp(target, prefixes[k], n) == 0 && target[n]) {
                x->kind = k;
                x->target = ir_dup(c, target + n);
            }
        }
        if (x->kind < 0) ERR(c, ir_key_pos(t, "target"), "RP1315", "target must be \"dir:<ID>\", \"file:<ID>\" or \"registry:<ID>\"");
        rp_mem_free(c->alloc, target);
    }
    x->sddl = ir_get_str(c, t, "sddl", true, NULL);
    if (x->sddl) {
        bool ok = strlen(x->sddl) >= 3 && strchr("DOGS", x->sddl[0]) && x->sddl[1] == ':';
        for (const char *p = x->sddl; ok && *p; ++p) ok = *p > ' ' && *p < 127 && *p != '[' && *p != ']';
        if (!ok) ERR(c, ir_key_pos(t, "sddl"), "RP1316", "sddl must be an SDDL string like \"D:PAI(A;OICI;FA;;;BA)\"");
    }
}

void ir_parse_font(ctx_t *c, const rp_ttable_t *t, rp_ir_font_t *x) {
    static const char *const keys[] = { "file", "title", NULL };
    ir_check_keys(c, t, keys);
    ir_check_id(c, t, 72);
    x->id = ir_dup(c, t->id);
    x->pos = t->pos;
    char *file = ir_get_str(c, t, "file", true, NULL);
    if (file) {
        if (strncmp(file, "file:", 5) != 0 || file[5] == '\0') ERR(c, ir_key_pos(t, "file"), "RP1315", "file must be \"file:<ID>\"");
        else x->file = ir_dup(c, file + 5);
        rp_mem_free(c->alloc, file);
    }
    x->title = ir_get_str(c, t, "title", false, NULL);
}

// [merge.ID] (RFC-0016 3).
void ir_parse_merge(ctx_t *c, const rp_ttable_t *t, rp_ir_merge_t *x) {
    static const char *const keys[] = { "source", "dir", "feature", "config", NULL };
    ir_check_keys(c, t, keys);
    ir_check_id(c, t, 72);
    x->id = ir_dup(c, t->id);
    x->msi_only = ir_get_bool(c, t, "msi-only", false);
    // config = ["Name=value", ...]: the answers to a configurable module's items (RFC-0017 C).
    const rp_tkey_t *cfg = ir_find_key(t, "config");
    if (cfg) {
        if (cfg->val.kind != RP_TV_ARRAY) {
            ERR(c, cfg->pos, "RP1306", "config is an array of \"Name=value\" strings");
        } else {
            x->config = rp_mem_alloc(c->alloc, cfg->val.count, sizeof *x->config);
            if (x->config == NULL) c->nomem = true;
            for (size_t k = 0; x->config && k < cfg->val.count; ++k) {
                const rp_tval_t *v = &cfg->val.items[k];
                char *s = v->kind == RP_TV_STRING ? ir_subst(c, v) : NULL;
                const char *eq = s ? strchr(s, '=') : NULL;
                if (eq == NULL || eq == s) {
                    ERR(c, cfg->pos, "RP1306", "config holds \"Name=value\" strings (got '%s')", s ? s : "?");
                    rp_mem_free(c->alloc, s);
                    continue;
                }
                x->config[x->config_count++] = s;
            }
        }
    }
    x->pos = t->pos;
    x->shown = ir_get_str(c, t, "source", true, NULL);
    x->dir = ir_get_str(c, t, "dir", true, NULL);
    x->feature = ir_get_str(c, t, "feature", false, NULL);
    if (x->shown && ir_source_path_ok(c, x->shown, ir_key_pos(t, "source"))) {
        x->source = ir_join(c, c->opt->source_dir ? c->opt->source_dir : ".", x->shown);
        uint64_t size;
        if (x->source && rp_pal_stat(c->alloc, x->source, &size) != RP_FS_FILE) {
            ERR(c, ir_key_pos(t, "source"), "RP1507", "merge module '%s' not found", x->shown);
        }
    }
}

// "file:<ID>" -> the ID (a copy), or NULL with RP1315.
static char *file_ref(ctx_t *c, const rp_ttable_t *t, const char *key, bool required) {
    char *v = ir_get_str(c, t, key, required, NULL);
    if (v == NULL) return NULL;
    char *id = NULL;
    if (strncmp(v, "file:", 5) != 0 || v[5] == '\0') ERR(c, ir_key_pos(t, key), "RP1315", "%s must be \"file:<ID>\" naming a [file.*] of this package", key);
    else id = ir_dup(c, v + 5);
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

// The prog-id rule shared by [assoc] and [com]: letters, digits, '.', '_', '-', no leading digit, <= 39.
static void check_prog_id(ctx_t *c, const rp_ttable_t *t, const char *p) {
    bool ok = strlen(p) <= 39 && p[0] && !(p[0] >= '0' && p[0] <= '9');
    for (const char *q = p; ok && *q; ++q) {
        ok = (*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') || (*q >= '0' && *q <= '9') || *q == '.' || *q == '_' || *q == '-';
    }
    if (!ok) ERR(c, ir_key_pos(t, "prog-id"), "RP1316", "prog-id must be letters, digits, '.', '_' or '-', not starting with a digit, at most 39 (got '%s')", p);
}

void ir_parse_assoc(ctx_t *c, const rp_ttable_t *t, rp_ir_assoc_t *x) {
    static const char *const keys[] = { "extension", "prog-id", "description", "target", "icon", "args", "msi-only", NULL };
    ir_check_keys(c, t, keys);
    ir_check_id(c, t, 64);
    x->id = ir_dup(c, t->id);
    x->pos = t->pos;
    x->extension = ir_get_str(c, t, "extension", true, NULL);
    if (x->extension && (x->extension[0] != '.' || strlen(x->extension) > 64 || !lower_name(x->extension + 1, "_-", 1))) {
        ERR(c, ir_key_pos(t, "extension"), "RP1316", "extension must be '.' and lower-case letters, digits, '_' or '-' (got '%s')", x->extension);
    }
    x->prog_id = ir_get_str(c, t, "prog-id", true, NULL);
    if (x->prog_id) check_prog_id(c, t, x->prog_id);
    x->description = ir_get_str(c, t, "description", false, NULL);
    x->target_file = file_ref(c, t, "target", true);
    x->icon_file = file_ref(c, t, "icon", false);
    x->args = ir_get_fmt(c, t, "args", false, NULL, IR_FMT_INSTALL);
    if (x->args == NULL) x->args = ir_dup(c, "\"%1\"");
}

void ir_parse_protocol(ctx_t *c, const rp_ttable_t *t, rp_ir_protocol_t *x) {
    static const char *const keys[] = { "name", "description", "target", "args", "msi-only", NULL };
    ir_check_keys(c, t, keys);
    ir_check_id(c, t, 64);
    x->id = ir_dup(c, t->id);
    x->pos = t->pos;
    x->name = ir_get_str(c, t, "name", true, NULL);
    if (x->name && (!(x->name[0] >= 'a' && x->name[0] <= 'z') || strlen(x->name) > 64 || !lower_name(x->name, "+-.", 2))) {
        ERR(c, ir_key_pos(t, "name"), "RP1316", "a scheme is a lower-case letter, then lower-case letters, digits, '+', '-' or '.' (got '%s')", x->name);
    }
    static const char *const taken[] = { "http", "https", "file", "ftp", "mailto", "ms-settings", "shell", NULL };
    if (x->name && ir_in_list(x->name, taken)) ERR(c, ir_key_pos(t, "name"), "RP1316", "'%s' belongs to Windows or the browsers", x->name);
    x->description = ir_get_str(c, t, "description", false, NULL);
    x->target_file = file_ref(c, t, "target", true);
    x->args = ir_get_fmt(c, t, "args", false, NULL, IR_FMT_INSTALL);
    if (x->args == NULL) x->args = ir_dup(c, "\"%1\"");
}

static char *guid_key(ctx_t *c, const rp_ttable_t *t, const char *key, bool required) {
    char *v = ir_get_str(c, t, key, required, NULL);
    if (v && !ir_guid_ok(v)) ERR(c, ir_key_pos(t, key), "RP1316", "%s must be a GUID like {12345678-...} (got '%s')", key, v);
    return v;
}

void ir_parse_com(ctx_t *c, const rp_ttable_t *t, rp_ir_com_t *x) {
    static const char *const keys[] = { "file", "class", "description", "threading", "args", "prog-id", "app-id", "surrogate",
                                        "typelib", "typelib-version", "typelib-file", "msi-only", NULL };
    ir_check_keys(c, t, keys);
    ir_check_id(c, t, 64);
    x->id = ir_dup(c, t->id);
    x->pos = t->pos;
    x->file = file_ref(c, t, "file", true);
    x->clsid = guid_key(c, t, "class", true);
    x->description = ir_get_str(c, t, "description", false, NULL);
    char *th = ir_get_str(c, t, "threading", false, NULL);
    static const char *const models[][2] = { { "sta", "Apartment" }, { "mta", "Free" }, { "both", "Both" }, { "neutral", "Neutral" } };
    for (size_t k = 0; th && k < 4; ++k) {
        if (strcmp(th, models[k][0]) == 0) x->threading = ir_dup(c, models[k][1]);
    }
    if (th && x->threading == NULL) ERR(c, ir_key_pos(t, "threading"), "RP1316", "threading must be \"sta\", \"mta\", \"both\" or \"neutral\" (got '%s')", th);
    rp_mem_free(c->alloc, th);
    x->args = ir_get_fmt(c, t, "args", false, NULL, IR_FMT_INSTALL);
    x->prog_id = ir_get_str(c, t, "prog-id", false, NULL);
    if (x->prog_id) check_prog_id(c, t, x->prog_id);
    x->app_id = guid_key(c, t, "app-id", false);
    x->surrogate = ir_get_bool(c, t, "surrogate", false);
    x->typelib = guid_key(c, t, "typelib", false);
    x->typelib_version = ir_get_str(c, t, "typelib-version", false, NULL);
    x->typelib_file = file_ref(c, t, "typelib-file", false);
    if (x->typelib_version) {
        // "major.minor", each 1 to 4 hex digits, as the TypeLib key spells them.
        const char *v = x->typelib_version, *dot = strchr(v, '.');
        bool ok = dot && dot > v && dot - v <= 4 && dot[1] && strlen(dot + 1) <= 4;
        for (const char *q = v; ok && *q; ++q) ok = q == dot || ir_is_hex(*q);
        if (!ok) ERR(c, ir_key_pos(t, "typelib-version"), "RP1316", "typelib-version is major.minor in hex digits, like \"1.0\" (got '%s')", v);
    }
    if (x->typelib == NULL && (x->typelib_version || x->typelib_file)) ERR(c, t->pos, "RP1316", "[com.%s]: typelib-version and typelib-file need typelib", t->id);
    if (x->typelib && x->typelib_version == NULL) x->typelib_version = ir_dup(c, "1.0");
    x->msi_only = ir_get_bool(c, t, "msi-only", false);
}

void ir_parse_handler(ctx_t *c, const rp_ttable_t *t, rp_ir_handler_t *x) {
    static const char *const keys[] = { "kind", "class", "types", "description", "msi-only", NULL };
    ir_check_keys(c, t, keys);
    ir_check_id(c, t, 48);
    x->id = ir_dup(c, t->id);
    x->pos = t->pos;
    char *kind = ir_get_str(c, t, "kind", true, NULL);
    if (kind && strcmp(kind, "preview") == 0) x->kind = RP_HANDLER_PREVIEW;
    else if (kind && strcmp(kind, "thumbnail") == 0) x->kind = RP_HANDLER_THUMBNAIL;
    else if (kind && strcmp(kind, "property") == 0) x->kind = RP_HANDLER_PROPERTY;
    else if (kind) ERR(c, ir_key_pos(t, "kind"), "RP1316", "kind must be \"preview\", \"thumbnail\" or \"property\" (got '%s')", kind);
    rp_mem_free(c->alloc, kind);
    x->clsid = guid_key(c, t, "class", true);
    x->description = ir_get_str(c, t, "description", false, NULL);
    x->msi_only = ir_get_bool(c, t, "msi-only", false);
    const rp_tkey_t *tk = ir_find_key(t, "types");
    if (tk == NULL || tk->val.kind != RP_TV_ARRAY || tk->val.count == 0) {
        ERR(c, tk ? tk->pos : t->pos, "RP1316", "[handler.%s]: types is a list of file types, like [\".txt\"]", t->id);
        return;
    }
    x->types = rp_mem_alloc(c->alloc, tk->val.count, sizeof *x->types);
    if (x->types == NULL) {
        c->nomem = true;
        return;
    }
    for (size_t k = 0; k < tk->val.count; ++k) {
        const rp_tval_t *v = &tk->val.items[k];
        char *ty = v->kind == RP_TV_STRING ? ir_subst(c, v) : NULL;
        if (!(ty && ty[0] == '.' && strlen(ty) <= 64 && lower_name(ty + 1, "_-", 1))) {
            ERR(c, tk->pos, "RP1316", "types: each is '.' and lower-case letters, digits, '_' or '-' (got '%s')", ty ? ty : "?");
        }
        x->types[x->type_count++] = ty;
    }
}

void ir_parse_msix_ext(ctx_t *c, const rp_ttable_t *t, rp_ir_msix_ext_t *x) {
    static const char *const keys[] = { "kind", "app", "alias", "task-id", "display-name", "enabled", "file", "direction",
                                        "protocol", "ports", "profile", "class", "threading", "args", "verb", "types", NULL };
    ir_check_keys(c, t, keys);
    ir_check_id(c, t, 72);
    x->id = ir_dup(c, t->id);
    x->pos = t->pos;
    char *kind = ir_get_str(c, t, "kind", true, NULL);
    x->app = ir_get_str(c, t, "app", false, NULL);
    x->enabled = ir_get_bool(c, t, "enabled", true);
    if (kind && strcmp(kind, "alias") == 0) {
        x->kind = RP_MSIX_EXT_ALIAS;
        x->alias = ir_get_str(c, t, "alias", true, NULL);
        size_t n = x->alias ? strlen(x->alias) : 0;
        bool ok = n > 4 && n <= 64 && strcmp(x->alias + n - 4, ".exe") == 0;
        for (size_t k = 0; ok && k < n; ++k) {
            char ch = x->alias[k];
            ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-';
        }
        if (x->alias && !ok) ERR(c, ir_key_pos(t, "alias"), "RP1316", "alias must be a file name ending in .exe: letters, digits, '.', '_', '-' (got '%s')", x->alias);
        if (ir_find_key(t, "task-id") || ir_find_key(t, "display-name") || ir_find_key(t, "enabled")) {
            ERR(c, t->pos, "RP1316", "task-id, display-name and enabled belong to kind = \"startup-task\"");
        }
    } else if (kind && strcmp(kind, "startup-task") == 0) {
        x->kind = RP_MSIX_EXT_STARTUP;
        x->task_id = ir_get_str(c, t, "task-id", false, NULL);
        if (x->task_id == NULL) x->task_id = ir_dup(c, t->id);
        x->display = ir_get_str(c, t, "display-name", false, NULL);
        if (ir_find_key(t, "alias")) ERR(c, ir_key_pos(t, "alias"), "RP1316", "alias belongs to kind = \"alias\"");
    } else if (kind && strcmp(kind, "firewall") == 0) {
        // RFC-0016 2: an inbound or outbound rule for a program of the package (desktop2:FirewallRules).
        x->kind = RP_MSIX_EXT_FIREWALL;
        x->file = file_ref(c, t, "file", false);
        char *dir = ir_get_str(c, t, "direction", true, NULL), *proto = ir_get_str(c, t, "protocol", true, NULL);
        if (dir && strcmp(dir, "in") != 0 && strcmp(dir, "out") != 0) {
            ERR(c, ir_key_pos(t, "direction"), "RP1316", "direction must be \"in\" or \"out\" (got '%s')", dir);
        }
        if (proto && strcmp(proto, "tcp") != 0 && strcmp(proto, "udp") != 0) {
            ERR(c, ir_key_pos(t, "protocol"), "RP1316", "protocol must be \"tcp\" or \"udp\" (got '%s')", proto);
        }
        x->outbound = dir && strcmp(dir, "out") == 0;
        x->udp = proto && strcmp(proto, "udp") == 0;
        rp_mem_free(c->alloc, dir);
        rp_mem_free(c->alloc, proto);
        char *ports = ir_get_str(c, t, "ports", false, NULL);
        if (ports) {
            unsigned lo = 0, hi = 0;
            int used = 0;
            bool ok = sscanf(ports, "%5u%n", &lo, &used) == 1 && used > 0;
            if (ok && ports[used] == '-') {
                int more = 0;
                ok = sscanf(ports + used + 1, "%5u%n", &hi, &more) == 1 && more > 0 && ports[used + 1 + more] == 0;
            } else if (ok) {
                hi = lo;
                ok = ports[used] == 0;
            }
            if (!ok || lo < 1 || hi > 65535 || lo > hi) {
                ERR(c, ir_key_pos(t, "ports"), "RP1316", "ports must be a port or a range like \"8000-8100\" within 1-65535 (got '%s')", ports);
            } else {
                x->port_min = lo;
                x->port_max = hi;
            }
            rp_mem_free(c->alloc, ports);
        }
        x->profile = ir_get_str(c, t, "profile", false, NULL);
        static const char *const profiles[] = { "all", "domain", "private", "public", NULL };
        if (x->profile && !ir_in_list(x->profile, profiles)) {
            ERR(c, ir_key_pos(t, "profile"), "RP1316", "profile must be \"all\", \"domain\", \"private\" or \"public\" (got '%s')", x->profile);
        }
        const char *other[] = { "alias", "task-id", "display-name", "enabled" };
        for (size_t k = 0; k < sizeof other / sizeof other[0]; ++k) {
            if (ir_find_key(t, other[k])) ERR(c, ir_key_pos(t, other[k]), "RP1316", "%s does not belong to kind = \"firewall\"", other[k]);
        }
    } else if (kind && (strcmp(kind, "com-server") == 0 || strcmp(kind, "toast") == 0 || strcmp(kind, "context-menu") == 0)) {
        // RFC-0016 2: a COM class of the package (com:ComServer), the toast activator (a COM class of
        // the application's program) and an Explorer context menu verb (a COM class in a DLL).
        x->kind = strcmp(kind, "com-server") == 0 ? RP_MSIX_EXT_COM : strcmp(kind, "toast") == 0 ? RP_MSIX_EXT_TOAST : RP_MSIX_EXT_CONTEXT_MENU;
        x->file = file_ref(c, t, "file", x->kind != RP_MSIX_EXT_TOAST);
        x->clsid = ir_get_str(c, t, "class", true, NULL);
        if (x->clsid && !ir_guid_ok(x->clsid)) ERR(c, ir_key_pos(t, "class"), "RP1316", "class must be a GUID like {12345678-...} (got '%s')", x->clsid);
        x->display = ir_get_str(c, t, "display-name", false, NULL);
        x->args = ir_get_str(c, t, "args", false, NULL);
        if (x->kind == RP_MSIX_EXT_TOAST && x->args == NULL) x->args = ir_dup(c, "-ToastActivated");
        char *th = ir_get_str(c, t, "threading", false, NULL);
        static const char *const models[][2] = { { "sta", "STA" }, { "mta", "MTA" }, { "both", "Both" }, { "neutral", "Neutral" } };
        if (th) {
            for (size_t k = 0; k < 4; ++k) {
                if (strcmp(th, models[k][0]) == 0) x->threading = ir_dup(c, models[k][1]);
            }
            if (x->threading == NULL) ERR(c, ir_key_pos(t, "threading"), "RP1316", "threading must be \"sta\", \"mta\", \"both\" or \"neutral\" (got '%s')", th);
            rp_mem_free(c->alloc, th);
        } else {
            x->threading = ir_dup(c, "STA");
        }
        if (x->kind == RP_MSIX_EXT_CONTEXT_MENU) {
            x->verb = ir_get_str(c, t, "verb", false, NULL);
            if (x->verb == NULL) x->verb = ir_dup(c, t->id);
            const rp_tkey_t *tk = ir_find_key(t, "types");
            if (tk == NULL || tk->val.kind != RP_TV_ARRAY || tk->val.count == 0) {
                ERR(c, tk ? tk->pos : t->pos, "RP1316", "[msix-extension.%s]: types is a list of file types, like [\".txt\", \"*\"]", t->id);
            } else {
                x->types = rp_mem_alloc(c->alloc, tk->val.count, sizeof *x->types);
                if (x->types == NULL) c->nomem = true;
                for (size_t k = 0; x->types && k < tk->val.count; ++k) {
                    const rp_tval_t *v = &tk->val.items[k];
                    char *ty = v->kind == RP_TV_STRING ? ir_subst(c, v) : NULL;
                    bool ok = ty && (strcmp(ty, "*") == 0 || (ty[0] == '.' && ty[1] && !strpbrk(ty + 1, ".\\")));
                    if (!ok) ERR(c, tk->pos, "RP1316", "types: each is \".ext\" or \"*\" (every file)");
                    x->types[x->type_count++] = ty;
                }
            }
        } else if (ir_find_key(t, "types") || ir_find_key(t, "verb")) {
            ERR(c, t->pos, "RP1316", "types and verb belong to kind = \"context-menu\"");
        }
        const char *other[] = { "alias", "task-id", "enabled", "direction", "protocol", "ports", "profile" };
        for (size_t k = 0; k < sizeof other / sizeof other[0]; ++k) {
            if (ir_find_key(t, other[k])) ERR(c, ir_key_pos(t, other[k]), "RP1316", "%s does not belong to kind = \"%s\"", other[k], kind);
        }
    } else if (kind) {
        ERR(c, ir_key_pos(t, "kind"), "RP1316", "kind must be \"alias\", \"startup-task\", \"firewall\", \"com-server\", \"toast\" or \"context-menu\" (got '%s')", kind);
    }
    rp_mem_free(c->alloc, kind);
}

void ir_parse_env(ctx_t *c, const rp_ttable_t *t, rp_ir_env_t *e) {
    static const char *const keys[] = { "name", "value", "mode", "keep", "feature", "when", NULL };
    ir_check_keys(c, t, keys);
    ir_check_id(c, t, 72);
    e->id = ir_dup(c, t->id);
    e->pos = t->pos;
    e->msi_only = ir_get_bool(c, t, "msi-only", false);
    e->when = ir_get_when(c, t);
    e->name = ir_get_str(c, t, "name", true, NULL);
    if (e->name && (strchr("=+-!*", e->name[0]) || strchr(e->name, '=') || ir_has_control(e->name))) {
        ERR(c, ir_key_pos(t, "name"), "RP1316", "variable name '%s' may not contain '=' or start with = + - ! *", e->name);
    }
    e->value = ir_get_fmt(c, t, "value", true, NULL, IR_FMT_INSTALL);
    if (e->value && strstr(e->value, "[~]")) {
        ERR(c, ir_key_pos(t, "value"), "RP1316", "write the value without '[~]'; mode = \"append\" or \"prepend\" adds it");
    }
    char *mode = ir_get_str(c, t, "mode", false, NULL);
    if (mode) {
        if (strcmp(mode, "set") == 0) e->mode = 0;
        else if (strcmp(mode, "append") == 0) e->mode = 1;
        else if (strcmp(mode, "prepend") == 0) e->mode = 2;
        else ERR(c, ir_key_pos(t, "mode"), "RP1316", "mode must be \"set\", \"append\" or \"prepend\"");
        rp_mem_free(c->alloc, mode);
    }
    e->keep = ir_get_bool(c, t, "keep", false);
    e->feature = ir_get_str(c, t, "feature", false, NULL);
}
