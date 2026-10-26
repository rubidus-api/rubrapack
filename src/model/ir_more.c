// src/model/ir_more.c - RFC-0031 wave 1: MSI tables that have no MSIX counterpart -
// [move.ID] (MoveFile), [odbc-driver.ID] / [odbc-source.ID] (the ODBC tables), [publish.ID]
// (PublishComponent), and their checks against the rest of the package.

#include "ir_int.h"
#include "rubrapack/msix.h"

#include <string.h>

// A "file:ID" value as the ID (NULL when absent; an error when malformed).
static char *file_ref(ctx_t *c, const rp_ttable_t *t, const char *key, bool required) {
    char *v = ir_get_str(c, t, key, required, NULL);
    if (v == NULL) return NULL;
    char *id = NULL;
    if (strncmp(v, "file:", 5) != 0 || v[5] == '\0') ERR(c, ir_key_pos(t, key), "RP1315", "%s must be \"file:<ID>\" naming a [file.*] of this package", key);
    else id = ir_dup(c, v + 5);
    rp_mem_free(c->alloc, v);
    return id;
}

// A list of "Key=Value" texts.
static void key_values(ctx_t *c, const rp_ttable_t *t, const char *key, char ***out, size_t *count) {
    const rp_tkey_t *tk = ir_find_key(t, key);
    *out = NULL;
    *count = 0;
    if (tk == NULL) return;
    if (tk->val.kind != RP_TV_ARRAY) {
        ERR(c, tk->pos, "RP1316", "%s is a list of \"Key=Value\" texts", key);
        return;
    }
    *out = rp_mem_alloc(c->alloc, tk->val.count + 1, sizeof **out);
    if (*out == NULL) {
        c->nomem = true;
        return;
    }
    for (size_t i = 0; i < tk->val.count; ++i) {
        char *v = tk->val.items[i].kind == RP_TV_STRING ? ir_subst(c, &tk->val.items[i]) : NULL;
        const char *eq = v ? strchr(v, '=') : NULL;
        if (v == NULL || eq == NULL || eq == v) {
            ERR(c, tk->pos, "RP1316", "%s is a list of \"Key=Value\" texts", key);
            rp_mem_free(c->alloc, v);
            continue;
        }
        (*out)[(*count)++] = v;
    }
}

void ir_parse_move(ctx_t *c, const rp_ttable_t *t, rp_ir_move_t *x) {
    static const char *const keys[] = { "from", "name", "to", "new-name", "copy", "with", "feature", NULL };
    ir_check_keys(c, t, keys);
    ir_check_id(c, t, 72);
    x->id = ir_dup(c, t->id);
    x->pos = t->pos;
    char *from = ir_get_str(c, t, "from", true, NULL);
    if (from && strncmp(from, "property:", 9) == 0 && from[9]) x->from_prop = ir_dup(c, from + 9);
    else if (from) x->from_dir = ir_dup(c, from);
    rp_mem_free(c->alloc, from);
    x->name = ir_get_str(c, t, "name", true, NULL);
    x->to_dir = ir_get_str(c, t, "to", true, NULL);
    x->new_name = ir_get_str(c, t, "new-name", false, NULL);
    if (x->new_name) ir_target_name_ok(c, x->new_name, ir_key_pos(t, "new-name"));
    if (x->new_name && x->name && strpbrk(x->name, "*?"))
        ERR(c, ir_key_pos(t, "new-name"), "RP1316", "[move.%s]: new-name names one file; name has wildcards", t->id);
    x->copy = ir_get_bool(c, t, "copy", false);
    x->with_file = file_ref(c, t, "with", false);
    x->feature = ir_get_str(c, t, "feature", false, NULL);
    if (x->with_file && x->feature) ERR(c, ir_key_pos(t, "feature"), "RP1316", "[move.%s]: feature goes with a component of its own, not with", t->id);
}

void ir_parse_odbc_driver(ctx_t *c, const rp_ttable_t *t, rp_ir_odbc_driver_t *x) {
    static const char *const keys[] = { "file", "setup", "description", "attributes", NULL };
    ir_check_keys(c, t, keys);
    ir_check_id(c, t, 72);
    x->id = ir_dup(c, t->id);
    x->pos = t->pos;
    x->file = file_ref(c, t, "file", true);
    x->setup = file_ref(c, t, "setup", false);
    x->description = ir_get_str(c, t, "description", true, NULL);
    key_values(c, t, "attributes", &x->attributes, &x->attribute_count);
}

void ir_parse_odbc_source(ctx_t *c, const rp_ttable_t *t, rp_ir_odbc_source_t *x) {
    static const char *const keys[] = { "driver", "driver-name", "name", "registration", "attributes", "with", NULL };
    ir_check_keys(c, t, keys);
    ir_check_id(c, t, 72);
    x->id = ir_dup(c, t->id);
    x->pos = t->pos;
    x->driver = ir_get_str(c, t, "driver", false, NULL);
    x->driver_name = ir_get_str(c, t, "driver-name", false, NULL);
    x->name = ir_get_str(c, t, "name", true, NULL);
    char *reg = ir_get_str(c, t, "registration", false, NULL);
    if (reg && strcmp(reg, "user") == 0) x->user = true;
    else if (reg && strcmp(reg, "machine") == 0) x->user = false;
    else if (reg) ERR(c, ir_key_pos(t, "registration"), "RP1316", "registration must be \"machine\" or \"user\" (got '%s')", reg);
    else x->user = c->ir->scope != 0;        // a per-user or dual package registers for the user by default
    rp_mem_free(c->alloc, reg);
    key_values(c, t, "attributes", &x->attributes, &x->attribute_count);
    x->with_file = file_ref(c, t, "with", false);
    if ((x->driver == NULL) == (x->driver_name == NULL))
        ERR(c, t->pos, "RP1316", "[odbc-source.%s] names its driver once: driver (an [odbc-driver]) or driver-name", t->id);
    else if (x->driver_name && x->with_file == NULL)
        ERR(c, t->pos, "RP1202", "[odbc-source.%s] with driver-name needs with = \"file:<ID>\": the component it is installed with", t->id);
}

void ir_parse_publish(ctx_t *c, const rp_ttable_t *t, rp_ir_publish_t *x) {
    static const char *const keys[] = { "file", "category", "qualifier", "data", NULL };
    ir_check_keys(c, t, keys);
    ir_check_id(c, t, 72);
    x->id = ir_dup(c, t->id);
    x->pos = t->pos;
    x->file = file_ref(c, t, "file", true);
    x->category = ir_get_str(c, t, "category", true, NULL);
    if (x->category && !ir_guid_ok(x->category))
        ERR(c, ir_key_pos(t, "category"), "RP1308", "%s must be a GUID like {12345678-...} (got '%s')", "category", x->category);
    x->qualifier = ir_get_str(c, t, "qualifier", true, NULL);
    x->data = ir_get_str(c, t, "data", false, NULL);
}

static const rp_ir_file_t *find_file(const rp_ir_t *ir, const char *id) {
    for (size_t i = 0; id && i < ir->file_count; ++i) {
        if (strcmp(ir->files[i].id, id) == 0) return &ir->files[i];
    }
    return NULL;
}

static bool find_feature(const rp_ir_t *ir, const char *id) {
    for (size_t i = 0; id && i < ir->feature_count; ++i) {
        if (strcmp(ir->features[i].id, id) == 0) return true;
    }
    return false;
}

static const rp_ir_feature_t *find_feature_ptr(const rp_ir_t *ir, const char *id) {
    for (size_t i = 0; id && i < ir->feature_count; ++i) {
        if (strcmp(ir->features[i].id, id) == 0) return &ir->features[i];
    }
    return NULL;
}

// A standard action name that before/after may reference, and the reserved sequence ranges a bare
// sequence number may not fall in.
static bool standard_action(const char *s) {
    static const char *const names[] = { "InstallInitialize", "ProcessComponents", "UnpublishComponents", "RemoveFiles",
        "InstallFiles", "DuplicateFiles", "MoveFiles", "WriteRegistryValues", "RegisterProduct", "PublishComponents",
        "InstallServices", "StartServices", "StopServices", "DeleteServices", "InstallODBC", "RemoveODBC", "RegisterFonts",
        "UnregisterFonts", "InstallFinalize", "CreateShortcuts", "RemoveShortcuts", "CreateFolders", "RemoveFolders",
        "RemoveRegistryValues", "SelfRegModules", "SelfUnregModules", "RemoveDuplicateFiles", NULL };
    for (size_t i = 0; names[i]; ++i) if (strcmp(names[i], s) == 0) return true;
    return false;
}

void ir_action_checks(ctx_t *c) {
    rp_ir_t *ir = c->ir;
    for (size_t k = 0; k < ir->action_count; ++k) {
        const rp_ir_action_t *a = &ir->actions[k];
        if (a->kind == RP_ACT_PAIR) continue;
        if (a->before && !standard_action(a->before)) ERR(c, a->pos, "RP1316", "%s: '%s' is not a standard action rubrapack places", "before", a->before);
        if (a->after && !standard_action(a->after)) ERR(c, a->pos, "RP1316", "%s: '%s' is not a standard action rubrapack places", "after", a->after);
        // A deferred action that runs an installed program or DLL cannot be placed before the files
        // are installed (sequence 4000); use before = "InstallFiles" or a later number.
        bool deferred = a->phase != RP_PHASE_IMMEDIATE;
        if (deferred && a->kind != RP_ACT_BINARY && a->sequence != 0 && a->sequence <= 4000)
            ERR(c, a->pos, "RP1316", "[action.%s]: a deferred action that runs an installed file must be after InstallFiles (sequence > 4000)", a->id);
    }
}

void ir_more_checks(ctx_t *c) {
    rp_ir_t *ir = c->ir;
    for (size_t k = 0; k < ir->move_count; ++k) {
        rp_ir_move_t *x = &ir->moves[k];
        // A component of its own takes the destination folder's feature, or Main when none is declared.
        if (x->with_file == NULL && x->feature == NULL && x->to_dir) {
            const rp_ir_dir_t *d = ir_find_dir(ir, x->to_dir);
            bool declared = false;
            for (size_t f = 0; f < ir->feature_count; ++f) declared |= !ir->features[f].implicit;
            if (d && d->feature) x->feature = ir_dup(c, d->feature);
            else if (!declared) x->feature = ir_dup(c, "Main");
            else ERR(c, x->pos, "RP1202", "[move.%s] needs a feature (or with = \"file:<ID>\")", x->id);
        }
        if (x->from_dir && !ir_find_dir(ir, x->from_dir)) ERR(c, x->pos, "RP1315", "dir '%s' is not a dir ID", x->from_dir);
        if (x->to_dir && !ir_find_dir(ir, x->to_dir)) ERR(c, x->pos, "RP1315", "dir '%s' is not a dir ID", x->to_dir);
        if (x->with_file && !find_file(ir, x->with_file)) ERR(c, x->pos, "RP1315", "%s: file '%s' is not a [file.*] of this package", "with", x->with_file);
        if (x->feature && !find_feature(ir, x->feature)) ERR(c, x->pos, "RP1307", "feature '%s' is not defined", x->feature);
    }
    for (size_t k = 0; k < ir->odbc_driver_count; ++k) {
        const rp_ir_odbc_driver_t *x = &ir->odbc_drivers[k];
        const rp_ir_file_t *f = find_file(ir, x->file);
        if (x->file && f == NULL) ERR(c, x->pos, "RP1315", "%s: file '%s' is not a [file.*] of this package", "file", x->file);
        if (x->setup && find_file(ir, x->setup) == NULL) ERR(c, x->pos, "RP1315", "%s: file '%s' is not a [file.*] of this package", "setup", x->setup);
    }
    for (size_t k = 0; k < ir->odbc_source_count; ++k) {
        const rp_ir_odbc_source_t *x = &ir->odbc_sources[k];
        bool found = x->driver == NULL;
        for (size_t j = 0; x->driver && j < ir->odbc_driver_count; ++j) found |= strcmp(ir->odbc_drivers[j].id, x->driver) == 0;
        if (!found) ERR(c, x->pos, "RP1307", "driver '%s' is not an [odbc-driver.*]", x->driver);
        if (x->with_file && !find_file(ir, x->with_file)) ERR(c, x->pos, "RP1315", "%s: file '%s' is not a [file.*] of this package", "with", x->with_file);
        if (x->user && ir->scope == 0) ERR(c, x->pos, "RP1316", "[odbc-source.%s]: a per-machine package registers data sources for the machine", x->id);
    }
    for (size_t k = 0; k < ir->publish_count; ++k) {
        const rp_ir_publish_t *x = &ir->publishes[k];
        if (x->file && !find_file(ir, x->file)) ERR(c, x->pos, "RP1315", "%s: file '%s' is not a [file.*] of this package", "file", x->file);
    }
    // RFC-0031 B12: the Windows 11 menu's identity package has one build-time family name, which an
    // instance transform does not change, so two instances would register the same menu package.
    // Until the menu data is made instance-aware, instances and [menu.*] are not combined.
    if (ir->instances > 1 && ir->menu_count > 0)
        ERR(c, ir->menus[0].pos, "RP1316", "instances is not yet combined with [menu.*] (the menu's identity package is one per build)");
    // RFC-0031 B10: a billboard names the feature whose installation shows it.
    for (size_t k = 0; k < ir->billboard_count; ++k) {
        const rp_ir_billboard_t *x = &ir->billboards[k];
        if (x->feature && !find_feature(ir, x->feature)) ERR(c, x->pos, "RP1307", "feature '%s' is not defined", x->feature);
    }
    // RFC-0031 B8: an advertised shortcut advertises its target file's feature; that feature may
    // not be one marked advertise = "disallow".
    for (size_t k = 0; k < ir->shortcut_count; ++k) {
        const rp_ir_shortcut_t *s = &ir->shortcuts[k];
        if (!s->advertise) continue;
        const rp_ir_file_t *tf = find_file(ir, s->target_file);
        const rp_ir_feature_t *f = tf ? find_feature_ptr(ir, tf->feature) : NULL;
        if (f && f->no_advertise)
            ERR(c, s->pos, "RP1316", "[shortcut.%s] advertises feature '%s', which is advertise = \"disallow\"", s->id, f->id);
    }
    for (size_t k = 0; k < ir->file_count; ++k) {
        const rp_ir_file_t *f = &ir->files[k];
        if (f->isolate_for == NULL) continue;
        const rp_ir_file_t *app = find_file(ir, f->isolate_for);
        if (app == NULL) ERR(c, f->pos, "RP1315", "%s: file '%s' is not a [file.*] of this package", "isolate-for", f->isolate_for);
        if (app && f->dir && app->dir && strcmp(f->dir, app->dir) == 0)     // a file without dir is refused elsewhere
            ERR(c, f->pos, "RP1316", "[file.%s] is in its program's folder already; isolate-for is for a shared DLL installed elsewhere", f->id);
    }
}

// RFC-0031 B9: an assembly's identity, as MsiAssemblyName lists it - for a .NET one from its CLI
// metadata (a shared one, in the GAC, must be strong-named), for a Win32 one from its manifest's
// assemblyIdentity (the manifest a [file] in the same folder and feature, which joins the DLL's
// component). assembly-for must name a program.
static void asm_name(ctx_t *c, rp_ir_file_t *f, size_t *n, const char *name, const char *value) {
    if (*n < 6 && value && *value) {
        f->asm_names[*n][0] = ir_dup(c, name);
        f->asm_names[*n][1] = ir_dup(c, value);
        ++*n;
    }
}

static void asm_identity(ctx_t *c, rp_ir_file_t *f) {
    rp_ir_t *ir = c->ir;
    if (f->assembly == RP_ASM_NONE || f->source_path == NULL) return;
    if (f->assembly_for) {
        const rp_ir_file_t *app = find_file(ir, f->assembly_for);
        if (app == NULL) ERR(c, f->pos, "RP1315", "%s: file '%s' is not a [file.*] of this package", "assembly-for", f->assembly_for);
        else if (app->pe_machine == 0 || app->pe_is_dll) ERR(c, f->pos, "RP1316", "%s: '%s' is not a program (.exe)", "assembly-for", f->assembly_for);
    }
    size_t n = 0;
    if (f->assembly == RP_ASM_DOTNET) {
        uint8_t *d = NULL;
        size_t len = 0;
        rp_pe_assembly_t a;
        if (rp_pal_read_file(c->alloc, f->source_path, 1u << 30, &d, &len) != PROVEN_OK || rp_pe_assembly(d, len, &a) != PROVEN_OK || !a.is_assembly) {
            ERR(c, f->pos, "RP1316", "[file.%s]: '%s' is not a .NET assembly (no CLI header with an Assembly row)", f->id, f->source);
        } else if (f->assembly_for == NULL && !a.has_token) {
            ERR(c, f->pos, "RP1316", "[file.%s]: a shared .NET assembly goes into the GAC, which takes strong-named ones only; sign it, or give assembly-for = \"file:<program>\"", f->id);
        } else {
            char ver[32], tok[17];
            snprintf(ver, sizeof ver, "%u.%u.%u.%u", a.version[0], a.version[1], a.version[2], a.version[3]);
            for (int i = 0; i < 8; ++i) snprintf(tok + 2 * i, 3, "%02X", a.token[i]);
            asm_name(c, f, &n, "Name", a.name);
            asm_name(c, f, &n, "Version", ver);
            asm_name(c, f, &n, "Culture", a.culture[0] ? a.culture : "neutral");
            if (a.has_token) asm_name(c, f, &n, "PublicKeyToken", tok);
            asm_name(c, f, &n, "ProcessorArchitecture", a.arch);
        }
        rp_mem_free(c->alloc, d);
        return;
    }
    if (f->manifest == NULL) return;        // refused when it was read
    rp_ir_file_t *m = NULL;
    for (size_t k = 0; k < ir->file_count; ++k) {
        if (ir->files[k].id && strcmp(ir->files[k].id, f->manifest) == 0) m = &ir->files[k];
    }
    if (m == NULL) {
        ERR(c, f->pos, "RP1315", "%s: file '%s' is not a [file.*] of this package", "manifest", f->manifest);
        return;
    }
    if (m->manifest_of || m->assembly != RP_ASM_NONE || m == f) {
        ERR(c, f->pos, "RP1316", "[file.%s]: the manifest '%s' is another assembly's or an assembly itself", f->id, f->manifest);
        return;
    }
    if (!f->dir || !m->dir || strcmp(f->dir, m->dir) != 0 || !f->feature || !m->feature || strcmp(f->feature, m->feature) != 0 ||
        (f->when == NULL) != (m->when == NULL) || (f->when && strcmp(f->when, m->when) != 0)) {
        ERR(c, f->pos, "RP1316", "[file.%s]: its manifest '%s' must be in the same dir and feature (and under the same when): the two are one component", f->id, f->manifest);
        return;
    }
    m->manifest_of = ir_dup(c, f->id);
    uint8_t *x = NULL;
    size_t len = 0;
    static const char *const attrs[][2] = { { "name", "name" }, { "version", "version" }, { "type", "type" },
                                            { "processorArchitecture", "processorArchitecture" }, { "publicKeyToken", "publicKeyToken" } };
    char v[256];
    if (m->source_path == NULL || rp_pal_read_file(c->alloc, m->source_path, 1u << 20, &x, &len) != PROVEN_OK ||
        !rp_xml_attr((const char *)x, len, "assemblyIdentity", "name", v, sizeof v) ||
        !rp_xml_attr((const char *)x, len, "assemblyIdentity", "version", v, sizeof v)) {
        ERR(c, f->pos, "RP1316", "[file.%s]: its manifest '%s' has no assemblyIdentity with a name and a version", f->id, f->manifest);
    } else {
        for (size_t i = 0; i < sizeof attrs / sizeof attrs[0]; ++i) {
            if (rp_xml_attr((const char *)x, len, "assemblyIdentity", attrs[i][0], v, sizeof v)) asm_name(c, f, &n, attrs[i][1], v);
            else if (strcmp(attrs[i][0], "type") == 0) asm_name(c, f, &n, "type", "win32");
        }
    }
    rp_mem_free(c->alloc, x);
}

// After the files' PE headers are read: what must be a DLL or a program is one.
void ir_more_pe_checks(ctx_t *c) {
    rp_ir_t *ir = c->ir;
    for (size_t k = 0; k < ir->odbc_driver_count; ++k) {
        const rp_ir_odbc_driver_t *x = &ir->odbc_drivers[k];
        const rp_ir_file_t *f = find_file(ir, x->file);
        if (f && !f->pe_is_dll) ERR(c, x->pos, "RP1316", "[odbc-driver.%s]: file '%s' is not a DLL", x->id, x->file);
    }
    for (size_t k = 0; k < ir->file_count; ++k) {
        const rp_ir_file_t *f = &ir->files[k], *app = f->isolate_for ? find_file(ir, f->isolate_for) : NULL;
        if (app && (app->pe_machine == 0 || app->pe_is_dll)) ERR(c, f->pos, "RP1316", "isolate-for: '%s' is not a program (.exe)", f->isolate_for);
    }
    for (size_t k = 0; k < ir->file_count; ++k) asm_identity(c, &ir->files[k]);
}

static void free_list(proven_allocator_t a, char **v, size_t n) {
    for (size_t i = 0; i < n; ++i) rp_mem_free(a, v[i]);
    rp_mem_free(a, v);
}

void ir_more_free(rp_ir_t *ir, proven_allocator_t a) {
    for (size_t k = 0; k < ir->move_count; ++k) {
        rp_ir_move_t *x = &ir->moves[k];
        char *xs[] = { x->id, x->from_dir, x->from_prop, x->name, x->to_dir, x->new_name, x->with_file, x->feature };
        for (size_t j = 0; j < sizeof xs / sizeof xs[0]; ++j) rp_mem_free(a, xs[j]);
    }
    rp_mem_free(a, ir->moves);
    for (size_t k = 0; k < ir->odbc_driver_count; ++k) {
        rp_ir_odbc_driver_t *x = &ir->odbc_drivers[k];
        char *xs[] = { x->id, x->file, x->setup, x->description };
        for (size_t j = 0; j < sizeof xs / sizeof xs[0]; ++j) rp_mem_free(a, xs[j]);
        free_list(a, x->attributes, x->attribute_count);
    }
    rp_mem_free(a, ir->odbc_drivers);
    for (size_t k = 0; k < ir->odbc_source_count; ++k) {
        rp_ir_odbc_source_t *x = &ir->odbc_sources[k];
        char *xs[] = { x->id, x->driver, x->driver_name, x->name, x->with_file };
        for (size_t j = 0; j < sizeof xs / sizeof xs[0]; ++j) rp_mem_free(a, xs[j]);
        free_list(a, x->attributes, x->attribute_count);
    }
    rp_mem_free(a, ir->odbc_sources);
    for (size_t k = 0; k < ir->publish_count; ++k) {
        rp_ir_publish_t *x = &ir->publishes[k];
        char *xs[] = { x->id, x->file, x->category, x->qualifier, x->data };
        for (size_t j = 0; j < sizeof xs / sizeof xs[0]; ++j) rp_mem_free(a, xs[j]);
    }
    rp_mem_free(a, ir->publishes);
}
