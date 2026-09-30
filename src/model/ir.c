// src/model/ir.c - `.rpk` AST -> checked package model (include/rubrapack/ir.h, RFC-0002): the build
// order and freeing. The table parsers, checks and dump are in the other src/model/ir_*.c files.

#include "ir_int.h"

// ---- tables ----------------------------------------------------------------------------------

static const char *const top_kinds[] = { "package", "define", "arp", "ui", "msix", NULL };
static const char *const item_kinds[] = { "feature", "dir", "file", "files", "folder", "property", "action", "registry",
                                          "shortcut", "remove", "copy", "env", "ini", "require", "search", "service", "font", "permission",
                                          "ui-text", "dialog", "dialog-control", "assoc", "protocol", "msix-extension", "merge", NULL };
static const char *const later_kinds[] = { NULL };
static const char *const all_kinds[] = { "package", "define", "arp", "property", "feature", "dir", "file", "files", "folder",
                                         "registry", "shortcut", "env", "ini", "service", "assoc", "protocol",
                                         "font", "permission", "require", "search", "remove", "copy", "action",
                                         "arp", "ui", "ui-text", "dialog", "dialog-control", "msix", "msix-app", "msix-extension", "merge", NULL };

bool ir_in_list(const char *s, const char *const *list) {
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

// ---- build -----------------------------------------------------------------------------------


proven_err_t rp_ir_build(proven_allocator_t alloc, const rp_tdoc_t *doc, const rp_ir_options_t *opt, rp_ir_t *ir,
                         rp_srcdiags_t *diags) {
    if (doc == NULL || opt == NULL || ir == NULL || diags == NULL) return PROVEN_ERR_INVALID_ARG;
    memset(ir, 0, sizeof *ir);
    ir->alloc = alloc;
    ctx_t c = { .alloc = alloc, .doc = doc, .opt = opt, .d = diags, .ir = ir };

    const rp_ttable_t *package = NULL;
    size_t nfeat = 0, ndir = 0, nfile = 0, nfolder = 0, nprop = 0, naction = 0, nreg = 0, nshort = 0, nrem = 0, ncopy = 0, nenv = 0, nini = 0, nreq = 0, nsearch = 0, nsvc = 0, nfont = 0, nperm = 0, nuitext = 0, ndlg = 0, ndctl = 0, nassoc = 0, nproto = 0, next_ = 0, nmerge = 0;
    const rp_ttable_t *uit = NULL;
    const rp_ttable_t *arp = NULL;
    for (size_t k = 0; k < doc->count; ++k) {
        const rp_ttable_t *t = &doc->tables[k];
        if (!ir_in_list(t->kind, all_kinds)) {
            const char *hint = ir_suggest(t->kind, all_kinds);
            ERR(&c, t->pos, "RP1201", "unknown table [%s%s%s]%s%s%s", t->kind, t->id ? "." : "", t->id ? t->id : "",
                hint ? " (did you mean [" : "", hint ? hint : "", hint ? "]?)" : "");
            continue;
        }
        if (ir_in_list(t->kind, later_kinds)) {
            ERR(&c, t->pos, "RP1901", "[%s%s] is not supported yet (planned for %s)", t->kind, t->id ? ".*" : "",
                phase_of(t->kind));
            continue;
        }
        bool top = ir_in_list(t->kind, top_kinds);
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
        else if (strcmp(t->kind, "merge") == 0) ++nmerge;
        else if (strcmp(t->kind, "assoc") == 0) ++nassoc;
        else if (strcmp(t->kind, "protocol") == 0) ++nproto;
        else if (strcmp(t->kind, "msix-extension") == 0) ++next_;
        else if (strcmp(t->kind, "permission") == 0) ++nperm;
        else if (strcmp(t->kind, "ui-text") == 0) ++nuitext;
        else if (strcmp(t->kind, "dialog") == 0) ++ndlg;
        else if (strcmp(t->kind, "dialog-control") == 0) ++ndctl;
        else if (strcmp(t->kind, "ui") == 0) uit = t;
        else if (strcmp(t->kind, "arp") == 0) arp = t;
        else if (strcmp(t->kind, "msix") == 0) ir_parse_msix(&c, t);
        else if (strcmp(t->kind, "msix-app") == 0) ir_parse_msix_app(&c, t);
        // RFC-0009 M6: what an MSIX cannot carry (yet) is an error there, unless msi-only = true.
        static const char *const msix_ok[] = { "package", "define", "dir", "file", "files", "feature", "property", "ui", "ui-text",
                                               "dialog", "dialog-control", "arp", "msix", "msix-app", "msix-extension", "registry",
                                               "assoc", "protocol", "shortcut", "font", "service", NULL };
        if (!ir_in_list(t->kind, msix_ok) && !ir_get_bool(&c, t, "msi-only", false)) {
            rp_ir_msix_block_t *nb = rp_mem_alloc(alloc, ir->msix_block_count + 1, sizeof *nb);
            if (nb == NULL) {
                c.nomem = true;
            } else {
                if (ir->msix_block_count) memcpy(nb, ir->msix_blocks, ir->msix_block_count * sizeof *nb);
                rp_mem_free(alloc, ir->msix_blocks);
                ir->msix_blocks = nb;
                nb[ir->msix_block_count++] = (rp_ir_msix_block_t){ ir_dup(&c, t->kind), ir_dup(&c, t->id), t->pos };
            }
        }
    }
    (void)item_kinds;
    if (c.define) {
        for (size_t k = 0; k < c.define->count; ++k) {
            const rp_tkey_t *key = &c.define->keys[k];
            if (!ir_valid_id(key->key, 64)) ERR(&c, key->pos, "RP1401", "'%s' is not a valid variable name", key->key);
            if (key->val.kind != RP_TV_STRING) ERR(&c, key->pos, "RP1306", "[define] values must be strings");
        }
    }
    if (package == NULL) {
        rp_pos_t top = { 1, 1 };
        ERR(&c, top, "RP1202", "the file needs a [package] table");
    } else {
        ir_parse_package(&c, package);
    }
    if (arp) ir_parse_arp(&c, arp);
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
    ir->merges = rp_mem_alloc(alloc, nmerge + 1, sizeof *ir->merges);
    if (ir->merges == NULL) c.nomem = true;
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
            ir_parse_feature(&c, t, f);
        } else if (strcmp(t->kind, "dir") == 0) {
            rp_ir_dir_t *d = ir_push_dir(&c);
            if (d) ir_parse_dir(&c, t, d);
        } else if (strcmp(t->kind, "file") == 0) {
            rp_ir_file_t *f = ir_push_file(&c);
            if (f) ir_parse_file(&c, t, f);
        } else if (strcmp(t->kind, "folder") == 0) {
            rp_ir_folder_t *f = &ir->folders[ir->folder_count++];
            memset(f, 0, sizeof *f);
            ir_parse_folder(&c, t, f);
        } else if (strcmp(t->kind, "property") == 0) {
            rp_ir_property_t *p = &ir->properties[ir->property_count++];
            memset(p, 0, sizeof *p);
            ir_parse_property(&c, t, p);
        } else if (strcmp(t->kind, "action") == 0) {
            rp_ir_action_t *a = &ir->actions[ir->action_count++];
            memset(a, 0, sizeof *a);
            ir_parse_action(&c, t, a);
        } else if (strcmp(t->kind, "registry") == 0) {
            rp_ir_registry_t *r = &ir->registries[ir->registry_count++];
            memset(r, 0, sizeof *r);
            ir_parse_registry(&c, t, r);
        } else if (strcmp(t->kind, "shortcut") == 0) {
            rp_ir_shortcut_t *sc = &ir->shortcuts[ir->shortcut_count++];
            memset(sc, 0, sizeof *sc);
            ir_parse_shortcut(&c, t, sc);
        } else if (strcmp(t->kind, "remove") == 0) {
            rp_ir_remove_t *r = &ir->removes[ir->remove_count++];
            memset(r, 0, sizeof *r);
            ir_parse_remove(&c, t, r);
        } else if (strcmp(t->kind, "copy") == 0) {
            rp_ir_copy_t *cp = &ir->copies[ir->copy_count++];
            memset(cp, 0, sizeof *cp);
            ir_parse_copy(&c, t, cp);
        } else if (strcmp(t->kind, "env") == 0) {
            rp_ir_env_t *e = &ir->envs[ir->env_count++];
            memset(e, 0, sizeof *e);
            ir_parse_env(&c, t, e);
        } else if (strcmp(t->kind, "ini") == 0) {
            rp_ir_ini_t *x = &ir->inis[ir->ini_count++];
            memset(x, 0, sizeof *x);
            ir_parse_ini(&c, t, x);
        } else if (strcmp(t->kind, "require") == 0) {
            rp_ir_require_t *r = &ir->requires[ir->require_count++];
            memset(r, 0, sizeof *r);
            ir_parse_require(&c, t, r);
        } else if (strcmp(t->kind, "search") == 0) {
            rp_ir_search_t *x = &ir->searches[ir->search_count++];
            memset(x, 0, sizeof *x);
            ir_parse_search(&c, t, x);
        } else if (strcmp(t->kind, "service") == 0) {
            rp_ir_service_t *x = &ir->services[ir->service_count++];
            memset(x, 0, sizeof *x);
            ir_parse_service(&c, t, x);
        } else if (strcmp(t->kind, "merge") == 0) {
            rp_ir_merge_t *x = &ir->merges[ir->merge_count++];
            memset(x, 0, sizeof *x);
            ir_parse_merge(&c, t, x);
        } else if (strcmp(t->kind, "font") == 0) {
            rp_ir_font_t *x = &ir->fonts[ir->font_count++];
            memset(x, 0, sizeof *x);
            ir_parse_font(&c, t, x);
        } else if (strcmp(t->kind, "assoc") == 0) {
            rp_ir_assoc_t *x = &ir->assocs[ir->assoc_count++];
            memset(x, 0, sizeof *x);
            ir_parse_assoc(&c, t, x);
        } else if (strcmp(t->kind, "protocol") == 0) {
            rp_ir_protocol_t *x = &ir->protocols[ir->protocol_count++];
            memset(x, 0, sizeof *x);
            ir_parse_protocol(&c, t, x);
        } else if (strcmp(t->kind, "msix-extension") == 0) {
            rp_ir_msix_ext_t *x = &ir->msix_exts[ir->msix_ext_count++];
            memset(x, 0, sizeof *x);
            ir_parse_msix_ext(&c, t, x);
        } else if (strcmp(t->kind, "permission") == 0) {
            rp_ir_permission_t *x = &ir->permissions[ir->permission_count++];
            memset(x, 0, sizeof *x);
            ir_parse_permission(&c, t, x);
        } else if (strcmp(t->kind, "ui-text") == 0) {
            rp_ir_ui_text_t *x = &ir->ui_texts[ir->ui_text_count++];
            memset(x, 0, sizeof *x);
            ir_parse_ui_text(&c, t, x);
        } else if (strcmp(t->kind, "dialog") == 0) {
            rp_ir_dialog_t *x = &ir->dialogs[ir->dialog_count++];
            memset(x, 0, sizeof *x);
            ir_parse_dialog(&c, t, x);
        } else if (strcmp(t->kind, "dialog-control") == 0) {
            rp_ir_dialog_control_t *x = &ir->dialog_controls[ir->dialog_control_count++];
            memset(x, 0, sizeof *x);
            ir_parse_dialog_control(&c, t, x);
        }
    }
    // Wildcards after every dir is known (their feature and the implicit sub folders).
    for (size_t k = 0; k < doc->count && !c.nomem; ++k) {
        const rp_ttable_t *t = &doc->tables[k];
        if (t->id && strcmp(t->kind, "files") == 0) ir_expand_files(&c, t);
    }
    if (!c.nomem && ir->feature_count == 0) {       // G2: one hidden default feature
        rp_ir_feature_t *f = &ir->features[ir->feature_count++];
        memset(f, 0, sizeof *f);
        f->id = ir_dup(&c, "Main");
        f->title = ir_dup(&c, ir->name ? ir->name : "Main");
        f->level = 1;
        f->hidden = true;
        f->implicit = true;
    }
    // Properties and actions in ID order: the output never depends on the order of tables.
    for (size_t i = 1; !c.nomem && i < ir->property_count; ++i) {
        for (size_t j = i; j > 0 && ir_cmp_str(ir->properties[j - 1].id, ir->properties[j].id) > 0; --j) {
            rp_ir_property_t t = ir->properties[j];
            ir->properties[j] = ir->properties[j - 1];
            ir->properties[j - 1] = t;
        }
    }
    for (size_t i = 1; !c.nomem && i < ir->action_count; ++i) {
        for (size_t j = i; j > 0 && ir_cmp_str(ir->actions[j - 1].id, ir->actions[j].id) > 0; --j) {
            rp_ir_action_t t = ir->actions[j];
            ir->actions[j] = ir->actions[j - 1];
            ir->actions[j - 1] = t;
        }
    }
    for (size_t i = 1; !c.nomem && i < ir->permission_count; ++i) {
        for (size_t j = i; j > 0 && ir_cmp_str(ir->permissions[j - 1].id, ir->permissions[j].id) > 0; --j) {
            rp_ir_permission_t t = ir->permissions[j];
            ir->permissions[j] = ir->permissions[j - 1];
            ir->permissions[j - 1] = t;
        }
    }
    for (size_t i = 1; !c.nomem && i < ir->font_count; ++i) {
        for (size_t j = i; j > 0 && ir_cmp_str(ir->fonts[j - 1].id, ir->fonts[j].id) > 0; --j) {
            rp_ir_font_t t = ir->fonts[j];
            ir->fonts[j] = ir->fonts[j - 1];
            ir->fonts[j - 1] = t;
        }
    }
    for (size_t i = 1; !c.nomem && i < ir->service_count; ++i) {
        for (size_t j = i; j > 0 && ir_cmp_str(ir->services[j - 1].id, ir->services[j].id) > 0; --j) {
            rp_ir_service_t t = ir->services[j];
            ir->services[j] = ir->services[j - 1];
            ir->services[j - 1] = t;
        }
    }
    for (size_t i = 1; !c.nomem && i < ir->require_count; ++i) {
        for (size_t j = i; j > 0 && ir_cmp_str(ir->requires[j - 1].id, ir->requires[j].id) > 0; --j) {
            rp_ir_require_t t = ir->requires[j];
            ir->requires[j] = ir->requires[j - 1];
            ir->requires[j - 1] = t;
        }
    }
    for (size_t i = 1; !c.nomem && i < ir->search_count; ++i) {
        for (size_t j = i; j > 0 && ir_cmp_str(ir->searches[j - 1].id, ir->searches[j].id) > 0; --j) {
            rp_ir_search_t t = ir->searches[j];
            ir->searches[j] = ir->searches[j - 1];
            ir->searches[j - 1] = t;
        }
    }
    for (size_t i = 1; !c.nomem && i < ir->ini_count; ++i) {
        for (size_t j = i; j > 0 && ir_cmp_str(ir->inis[j - 1].id, ir->inis[j].id) > 0; --j) {
            rp_ir_ini_t t = ir->inis[j];
            ir->inis[j] = ir->inis[j - 1];
            ir->inis[j - 1] = t;
        }
    }
    for (size_t i = 1; !c.nomem && i < ir->env_count; ++i) {
        for (size_t j = i; j > 0 && ir_cmp_str(ir->envs[j - 1].id, ir->envs[j].id) > 0; --j) {
            rp_ir_env_t t = ir->envs[j];
            ir->envs[j] = ir->envs[j - 1];
            ir->envs[j - 1] = t;
        }
    }
    for (size_t i = 1; !c.nomem && i < ir->remove_count; ++i) {
        for (size_t j = i; j > 0 && ir_cmp_str(ir->removes[j - 1].id, ir->removes[j].id) > 0; --j) {
            rp_ir_remove_t t = ir->removes[j];
            ir->removes[j] = ir->removes[j - 1];
            ir->removes[j - 1] = t;
        }
    }
    for (size_t i = 1; !c.nomem && i < ir->copy_count; ++i) {
        for (size_t j = i; j > 0 && ir_cmp_str(ir->copies[j - 1].id, ir->copies[j].id) > 0; --j) {
            rp_ir_copy_t t = ir->copies[j];
            ir->copies[j] = ir->copies[j - 1];
            ir->copies[j - 1] = t;
        }
    }
    for (size_t i = 1; !c.nomem && i < ir->shortcut_count; ++i) {
        for (size_t j = i; j > 0 && ir_cmp_str(ir->shortcuts[j - 1].id, ir->shortcuts[j].id) > 0; --j) {
            rp_ir_shortcut_t t = ir->shortcuts[j];
            ir->shortcuts[j] = ir->shortcuts[j - 1];
            ir->shortcuts[j - 1] = t;
        }
    }
#define SORT_BY_ID(arr, count, type)                                                                \
    for (size_t i = 1; !c.nomem && i < (count); ++i) {                                              \
        for (size_t j = i; j > 0 && ir_cmp_str((arr)[j - 1].id, (arr)[j].id) > 0; --j) {               \
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
        for (size_t j = i; j > 0 && ir_cmp_str(ir->registries[j - 1].id, ir->registries[j].id) > 0; --j) {
            rp_ir_registry_t t = ir->registries[j];
            ir->registries[j] = ir->registries[j - 1];
            ir->registries[j - 1] = t;
        }
    }
    if (!c.nomem && package) ir_ui_checks(&c, uit, package);
    for (size_t i = 1; !c.nomem && i < ir->ui_text_count; ++i) {
        for (size_t j = i; j > 0 && ir_cmp_str(ir->ui_texts[j - 1].id, ir->ui_texts[j].id) > 0; --j) {
            rp_ir_ui_text_t t = ir->ui_texts[j];
            ir->ui_texts[j] = ir->ui_texts[j - 1];
            ir->ui_texts[j - 1] = t;
        }
    }
    for (size_t i = 1; !c.nomem && i < ir->dialog_count; ++i) {
        for (size_t j = i; j > 0 && ir_cmp_str(ir->dialogs[j - 1].id, ir->dialogs[j].id) > 0; --j) {
            rp_ir_dialog_t t = ir->dialogs[j];
            ir->dialogs[j] = ir->dialogs[j - 1];
            ir->dialogs[j - 1] = t;
        }
    }
    for (size_t i = 1; !c.nomem && i < ir->dialog_control_count; ++i) {
        for (size_t j = i; j > 0 && ir_cmp_str(ir->dialog_controls[j - 1].id, ir->dialog_controls[j].id) > 0; --j) {
            rp_ir_dialog_control_t t = ir->dialog_controls[j];
            ir->dialog_controls[j] = ir->dialog_controls[j - 1];
            ir->dialog_controls[j - 1] = t;
        }
    }
    if (!c.nomem && opt->nfc) ir_nfc_names(&c);
    if (!c.nomem) ir_dialog_checks(&c);
    if (!c.nomem) ir_class_checks(&c);
    if (!c.nomem) ir_cross_checks(&c);
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
        rp_mem_free(a, f->when);
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
        char *fs[] = { f->id, f->dir, f->source, f->source_path, f->name, f->feature, f->component_guid, f->when };
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
        char *rs[] = { r->id, r->key, r->name, r->value, r->with_file, r->feature, r->when };
        for (size_t j = 0; j < sizeof rs / sizeof rs[0]; ++j) rp_mem_free(a, rs[j]);
        for (size_t j = 0; j < r->item_count; ++j) rp_mem_free(a, r->items[j]);
        rp_mem_free(a, r->items);
    }
    rp_mem_free(a, ir->registries);
    for (size_t k = 0; k < ir->shortcut_count; ++k) {
        rp_ir_shortcut_t *sc = &ir->shortcuts[k];
        char *ss[] = { sc->id, sc->dir, sc->name, sc->target_file, sc->args, sc->description, sc->working_dir, sc->icon_source,
                       sc->icon_shown, sc->when };
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
        char *xs[] = { e->id, e->name, e->value, e->feature, e->when };
        for (size_t j = 0; j < sizeof xs / sizeof xs[0]; ++j) rp_mem_free(a, xs[j]);
    }
    rp_mem_free(a, ir->envs);
    for (size_t k = 0; k < ir->ini_count; ++k) {
        rp_ir_ini_t *x = &ir->inis[k];
        char *xs[] = { x->id, x->dir, x->file, x->section, x->key, x->value, x->feature, x->when };
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
    for (size_t k = 0; k < ir->merge_count; ++k) {
        rp_ir_merge_t *x = &ir->merges[k];
        char *xs[] = { x->id, x->source, x->shown, x->dir, x->feature };
        for (size_t j = 0; j < sizeof xs / sizeof xs[0]; ++j) rp_mem_free(a, xs[j]);
    }
    rp_mem_free(a, ir->merges);
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
        char *xs[] = { x->id, x->app, x->alias, x->task_id, x->display, x->file, x->profile, x->clsid, x->threading, x->args, x->verb };
        for (size_t j = 0; j < sizeof xs / sizeof xs[0]; ++j) rp_mem_free(a, xs[j]);
        for (size_t j = 0; j < x->type_count; ++j) rp_mem_free(a, x->types[j]);
        rp_mem_free(a, x->types);
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
    char *us[] = { ir->license_source, ir->license_shown, ir->banner_source, ir->ui_install_dir, ir->ui_launch_file, ir->ui_launch_args };
    for (size_t k = 0; k < sizeof us / sizeof us[0]; ++k) rp_mem_free(a, us[k]);
    rp_mem_free(a, ir->removes);
    rp_mem_free(a, ir->copies);
    rp_mem_free(a, ir->properties);
    rp_mem_free(a, ir->actions);
    rp_mem_free(a, ir->arp_help);
    rp_mem_free(a, ir->arp_icon_source);
    rp_mem_free(a, ir->arp_icon_shown);
    rp_mem_free(a, ir->arp_about);
    rp_mem_free(a, ir->msix_identity_name);
    rp_mem_free(a, ir->msix_publisher);
    rp_mem_free(a, ir->msix_publisher_display);
    rp_mem_free(a, ir->msix_min_version);
    rp_mem_free(a, ir->msix_appinstaller_uri);
    rp_mem_free(a, ir->msix_package_uri);
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
