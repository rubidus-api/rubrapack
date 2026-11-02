// src/model/ir.c - `.toml` AST -> checked package model (include/rubrapack/ir.h, RFC-0002): the build
// order and freeing. The table parsers, checks and dump are in the other src/model/ir_*.c files.

#include "ir_int.h"

// ---- tables ----------------------------------------------------------------------------------

static const char *const top_kinds[] = { "package", "module", "define", "arp", "ui", "msix", "msix-psf", "winget", NULL };
static const char *const item_kinds[] = { "feature", "dir", "file", "files", "folder", "property", "action", "registry",
                                          "shortcut", "remove", "copy", "env", "ini", "require", "search", "service", "font", "permission",
                                          "ui-text", "dialog", "dialog-control", "billboard", "assoc", "protocol", "com", "handler", "menu", "other-copy", "move", "odbc-driver", "odbc-source", "publish", "msix-extension", "msix-dependency", "merge", NULL };
static const char *const later_kinds[] = { NULL };
static const char *const all_kinds[] = { "package", "module", "define", "arp", "property", "feature", "dir", "file", "files", "folder",
                                         "registry", "shortcut", "env", "ini", "service", "assoc", "protocol", "com", "handler", "menu", "other-copy",
                                         "move", "odbc-driver", "odbc-source", "publish", "font", "permission", "require", "search", "remove", "copy", "action",
                                         "arp", "ui", "ui-text", "dialog", "dialog-control", "billboard", "msix", "msix-app", "msix-extension", "msix-dependency", "msix-psf", "winget", "merge", NULL };

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

typedef struct { const char *id; size_t at; } idat_t;      // an element's ID and place

static int cmp_idat(const void *x, const void *y) {
    const idat_t *a = x, *b = y;
    int r = ir_cmp_str(RP_V(a->id), RP_V(b->id));
    return r ? r : a->at < b->at ? -1 : a->at > b->at;      // equal IDs keep their order
}

// Sorts n elements of `size` bytes by the `const char *` ID at id_off (NULL as ""), stably.
static void sort_by_id(ctx_t *c, void *base, size_t n, size_t size, size_t id_off) {
    if (c->nomem || n < 2) return;
    idat_t *ord = rp_mem_alloc(c->alloc, n, sizeof *ord);
    unsigned char *tmp = rp_mem_alloc(c->alloc, n, size), *b = base;
    if (ord == NULL || tmp == NULL) {
        rp_mem_free(c->alloc, ord);
        rp_mem_free(c->alloc, tmp);
        c->nomem = true;
        return;
    }
    for (size_t i = 0; i < n; ++i) {
        memcpy(&ord[i].id, b + i * size + id_off, sizeof ord[i].id);
        ord[i].at = i;
    }
    rp_sort(ord, n, sizeof *ord, cmp_idat);
    for (size_t i = 0; i < n; ++i) memcpy(tmp + i * size, b + ord[i].at * size, size);
    memcpy(b, tmp, n * size);
    rp_mem_free(c->alloc, ord);
    rp_mem_free(c->alloc, tmp);
}

#define SORT_BY_ID(arr, count, type) sort_by_id(&c, (arr), (count), sizeof(type), offsetof(type, id))


proven_err_t rp_ir_build(proven_allocator_t alloc, const rp_tdoc_t *doc, const rp_ir_options_t *opt, rp_ir_t *ir,
                         rp_srcdiags_t *diags) {
    if (doc == NULL || opt == NULL || ir == NULL || diags == NULL) return PROVEN_ERR_INVALID_ARG;
    memset(ir, 0, sizeof *ir);
    ir->alloc = alloc;
    ctx_t c = { .alloc = alloc, .doc = doc, .opt = opt, .d = diags, .ir = ir };

    ir_check_format(&c);
    const rp_ttable_t *package = NULL;
    size_t nfeat = 0, ndir = 0, nfile = 0, nfolder = 0, nprop = 0, naction = 0, nreg = 0, nshort = 0, nrem = 0, ncopy = 0, nenv = 0, nini = 0, nreq = 0, nsearch = 0, nsvc = 0, nfont = 0, nperm = 0, nuitext = 0, ndlg = 0, ndctl = 0, nassoc = 0, nproto = 0, ncom = 0, nhandler = 0, nhandler_types = 0, next_ = 0, nmerge = 0, nmenu = 0, nmenu_on = 0, nother = 0, nmove = 0, nodbcd = 0, nodbcs = 0, npublish = 0, nbboard = 0;
    const rp_ttable_t *uit = NULL;
    const rp_ttable_t *arp = NULL;
    for (size_t k = 0; k < doc->count; ++k) {
        const rp_ttable_t *t = &doc->tables[k];
        if (!ir_in_list(rp_cs(t->kind), all_kinds)) {
            const char *hint = ir_suggest(rp_cs(t->kind), all_kinds);
            if (hint) ERR(&c, t->pos, "RP1201", "unknown table [%s%s%s] (did you mean [%s]?)", rp_cs(t->kind), t->id.ptr ? "." : "", t->id.ptr ? rp_cs(t->id) : "", hint);
            else ERR(&c, t->pos, "RP1201", "unknown table [%s%s%s]", rp_cs(t->kind), t->id.ptr ? "." : "", t->id.ptr ? rp_cs(t->id) : "");
            continue;
        }
        if (ir_in_list(rp_cs(t->kind), later_kinds)) {
            ERR(&c, t->pos, "RP1901", "[%s%s] is not supported yet (planned for %s)", rp_cs(t->kind), t->id.ptr ? ".*" : "",
                phase_of(rp_cs(t->kind)));
            continue;
        }
        bool top = ir_in_list(rp_cs(t->kind), top_kinds);
        if (top && t->id.ptr) {
            ERR(&c, t->pos, "RP1201", "[%s] takes no ID: write [%s]", rp_cs(t->kind), rp_cs(t->kind));
            continue;
        }
        if (!top && t->id.ptr == NULL) {
            ERR(&c, t->pos, "RP1201", "[%s] needs an ID: write [%s.ID]", rp_cs(t->kind), rp_cs(t->kind));
            continue;
        }
        if (RP_EQ(t->kind, "package") || RP_EQ(t->kind, "module")) {
            if (package) ERR(&c, t->pos, "RP1201", "a source holds one [package] or one [module], not both");
            package = t;
        }
        else if (RP_EQ(t->kind, "define")) c.define = t;
        else if (RP_EQ(t->kind, "feature")) ++nfeat;
        else if (RP_EQ(t->kind, "dir")) ++ndir;
        else if (RP_EQ(t->kind, "file")) ++nfile;
        else if (RP_EQ(t->kind, "folder")) ++nfolder;
        else if (RP_EQ(t->kind, "property")) ++nprop;
        else if (RP_EQ(t->kind, "action")) ++naction;
        else if (RP_EQ(t->kind, "registry")) ++nreg;
        else if (RP_EQ(t->kind, "shortcut")) ++nshort;
        else if (RP_EQ(t->kind, "remove")) ++nrem;
        else if (RP_EQ(t->kind, "copy")) ++ncopy;
        else if (RP_EQ(t->kind, "env")) ++nenv;
        else if (RP_EQ(t->kind, "ini")) ++nini;
        else if (RP_EQ(t->kind, "require")) ++nreq;
        else if (RP_EQ(t->kind, "search")) ++nsearch;
        else if (RP_EQ(t->kind, "service")) ++nsvc;
        else if (RP_EQ(t->kind, "font")) ++nfont;
        else if (RP_EQ(t->kind, "merge")) ++nmerge;
        else if (RP_EQ(t->kind, "assoc")) ++nassoc;
        else if (RP_EQ(t->kind, "protocol")) ++nproto;
        else if (RP_EQ(t->kind, "com")) ++ncom;
        else if (RP_EQ(t->kind, "handler")) {
            ++nhandler;
            const rp_tkey_t *tk = ir_find_key(t, "types");
            nhandler_types += tk && tk->val.kind == RP_TV_ARRAY ? tk->val.count : 0;
        }
        else if (RP_EQ(t->kind, "menu")) {
            ++nmenu;
            const rp_tkey_t *tk = ir_find_key(t, "on");
            nmenu_on += tk && tk->val.kind == RP_TV_ARRAY ? tk->val.count : 1;
        }
        else if (RP_EQ(t->kind, "msix-extension")) ++next_;
        else if (RP_EQ(t->kind, "other-copy")) ++nother;
        else if (RP_EQ(t->kind, "move")) ++nmove;
        else if (RP_EQ(t->kind, "odbc-driver")) ++nodbcd;
        else if (RP_EQ(t->kind, "odbc-source")) ++nodbcs;
        else if (RP_EQ(t->kind, "publish")) ++npublish;
        else if (RP_EQ(t->kind, "permission")) ++nperm;
        else if (RP_EQ(t->kind, "ui-text")) ++nuitext;
        else if (RP_EQ(t->kind, "dialog")) ++ndlg;
        else if (RP_EQ(t->kind, "dialog-control")) ++ndctl;
        else if (RP_EQ(t->kind, "billboard")) ++nbboard;
        else if (RP_EQ(t->kind, "ui")) uit = t;
        else if (RP_EQ(t->kind, "arp")) arp = t;
        else if (RP_EQ(t->kind, "msix")) ir_parse_msix(&c, t);
        else if (RP_EQ(t->kind, "msix-app")) ir_parse_msix_app(&c, t);
        else if (RP_EQ(t->kind, "msix-dependency")) ir_parse_msix_dep(&c, t);
        else if (RP_EQ(t->kind, "msix-psf")) ir_parse_msix_psf(&c, t);
        else if (RP_EQ(t->kind, "winget")) ir_parse_winget(&c, t);
        // RFC-0009 M6: what an MSIX cannot carry (yet) is an error there, unless msi-only = true.
        static const char *const msix_ok[] = { "package", "define", "dir", "file", "files", "feature", "property", "ui", "ui-text",
                                               "dialog", "dialog-control", "arp", "msix", "msix-app", "msix-extension", "registry",
                                               "assoc", "protocol", "com", "handler", "menu", "other-copy", "shortcut", "font", "service", "msix-dependency", "msix-psf", "winget", "copy", "ini", "env", "merge", NULL };
        if (!ir_in_list(rp_cs(t->kind), msix_ok) && !ir_get_bool(&c, t, "msi-only", false)) {
            rp_ir_msix_block_t *nb = rp_mem_alloc(alloc, ir->msix_block_count + 1, sizeof *nb);
            if (nb == NULL) {
                c.nomem = true;
            } else {
                if (ir->msix_block_count) memcpy(nb, ir->msix_blocks, ir->msix_block_count * sizeof *nb);
                rp_mem_free(alloc, ir->msix_blocks);
                ir->msix_blocks = nb;
                nb[ir->msix_block_count++] = (rp_ir_msix_block_t){ ir_dup_v(&c, t->kind), ir_dup_v(&c, t->id), t->pos };
            }
        }
    }
    (void)item_kinds;
    if (c.define) {
        for (size_t k = 0; k < c.define->count; ++k) {
            const rp_tkey_t *key = &c.define->keys[k];
            if (!ir_valid_id(rp_cs(key->key), 64)) ERR(&c, key->pos, "RP1401", "'%s' is not a valid variable name", rp_cs(key->key));
            else if (ir_windows_name(rp_cs(key->key))) ERR(&c, key->pos, "RP1404", "'%s' is a Windows name; give the variable another name", rp_cs(key->key));
            for (size_t j = 0; j < doc->count; ++j) {
                const rp_ttable_t *dt = &doc->tables[j];
                if (dt->id.ptr && RP_EQ(dt->kind, "dir") && RP_EQ(dt->id, key->key)) {
                    ERR(&c, key->pos, "RP1404", "'%s' is also a dir ID; give the variable another name", rp_cs(key->key));
                }
            }
            if (key->val.kind != RP_TV_STRING) ERR(&c, key->pos, "RP1306", "[define] values must be strings");
        }
    }
    for (size_t k = 0; k < opt->define_count; ++k) {
        if (ir_windows_name(rp_cs(opt->defines[k].name))) {
            ERR(&c, ((rp_pos_t){ 1, 1 }), "RP1404", "-D %s: '%s' is a Windows name; give the variable another name",
                rp_cs(opt->defines[k].name), rp_cs(opt->defines[k].name));
        }
    }
    if (package == NULL) {
        rp_pos_t top = { 1, 1 };
        ERR(&c, top, "RP1202", "the file needs a [package] table (or [module] for a merge module)");
    } else if (RP_EQ(package->kind, "module")) {
        ir_parse_module(&c, package);
        // A merge module carries files, folders and the settings that go with them; features,
        // dialogs and the rest belong to the package that merges it.
        static const char *const module_ok[] = { "module", "define", "dir", "file", "files", "folder", "registry", "env", "ini",
                                                 "remove", "copy", NULL };
        for (size_t k = 0; k < doc->count; ++k) {
            const rp_ttable_t *t = &doc->tables[k];
            if (ir_in_list(rp_cs(t->kind), all_kinds) && !ir_in_list(rp_cs(t->kind), module_ok)) {
                ERR(&c, t->pos, "RP1201", "[%s%s] cannot go into a merge module; the package that merges it has it", rp_cs(t->kind), t->id.ptr ? ".*" : "");
            }
        }
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
    // [assoc], [protocol] and [com] add their HKCR values for the MSI (add_class_values), and each
    // [com] an [msix-extension] of kind com-server for the MSIX.
    ir->registries = rp_mem_alloc(alloc, nreg + 7 * nassoc + 4 * nproto + 16 * ncom + 4 * nhandler + 2 * nhandler_types + RP_MENU_VALUES * nmenu * (nmenu_on + 1) + nmenu * (10 + RP_UI_LANG_MAX) + 1,
                                    sizeof *ir->registries);
    ir->menus = rp_mem_alloc(alloc, nmenu + 1, sizeof *ir->menus);
    ir->others = rp_mem_alloc(alloc, nother + 1, sizeof *ir->others);
    ir->moves = rp_mem_alloc(alloc, nmove + 1, sizeof *ir->moves);
    ir->odbc_drivers = rp_mem_alloc(alloc, nodbcd + 1, sizeof *ir->odbc_drivers);
    ir->odbc_sources = rp_mem_alloc(alloc, nodbcs + 1, sizeof *ir->odbc_sources);
    ir->publishes = rp_mem_alloc(alloc, npublish + 1, sizeof *ir->publishes);
    if (ir->menus == NULL || ir->others == NULL || ir->moves == NULL || ir->odbc_drivers == NULL || ir->odbc_sources == NULL ||
        ir->publishes == NULL) c.nomem = true;
    ir->assocs = rp_mem_alloc(alloc, nassoc + 1, sizeof *ir->assocs);
    ir->protocols = rp_mem_alloc(alloc, nproto + 1, sizeof *ir->protocols);
    ir->coms = rp_mem_alloc(alloc, ncom + 1, sizeof *ir->coms);
    ir->handlers = rp_mem_alloc(alloc, nhandler + 1, sizeof *ir->handlers);
    if (ir->handlers == NULL) c.nomem = true;
    ir->msix_exts = rp_mem_alloc(alloc, next_ + ncom + nmenu + 1, sizeof *ir->msix_exts);
    if (ir->assocs == NULL || ir->protocols == NULL || ir->coms == NULL || ir->msix_exts == NULL) c.nomem = true;
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
    ir->billboards = rp_mem_alloc(alloc, nbboard + 1, sizeof *ir->billboards);
    if (ir->ui_texts == NULL || ir->dialogs == NULL || ir->dialog_controls == NULL || ir->billboards == NULL) c.nomem = true;
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
        if (t->id.ptr == NULL) continue;
        if (RP_EQ(t->kind, "feature")) {
            rp_ir_feature_t *f = &ir->features[ir->feature_count++];
            memset(f, 0, sizeof *f);
            ir_parse_feature(&c, t, f);
        } else if (RP_EQ(t->kind, "dir")) {
            rp_ir_dir_t *d = ir_push_dir(&c);
            if (d) ir_parse_dir(&c, t, d);
        } else if (RP_EQ(t->kind, "file")) {
            rp_ir_file_t *f = ir_push_file(&c);
            if (f) ir_parse_file(&c, t, f);
        } else if (RP_EQ(t->kind, "folder")) {
            rp_ir_folder_t *f = &ir->folders[ir->folder_count++];
            memset(f, 0, sizeof *f);
            ir_parse_folder(&c, t, f);
        } else if (RP_EQ(t->kind, "property")) {
            rp_ir_property_t *p = &ir->properties[ir->property_count++];
            memset(p, 0, sizeof *p);
            ir_parse_property(&c, t, p);
        } else if (RP_EQ(t->kind, "action")) {
            rp_ir_action_t *a = &ir->actions[ir->action_count++];
            memset(a, 0, sizeof *a);
            ir_parse_action(&c, t, a);
        } else if (RP_EQ(t->kind, "registry")) {
            rp_ir_registry_t *r = &ir->registries[ir->registry_count++];
            memset(r, 0, sizeof *r);
            ir_parse_registry(&c, t, r);
        } else if (RP_EQ(t->kind, "shortcut")) {
            rp_ir_shortcut_t *sc = &ir->shortcuts[ir->shortcut_count++];
            memset(sc, 0, sizeof *sc);
            ir_parse_shortcut(&c, t, sc);
        } else if (RP_EQ(t->kind, "remove")) {
            rp_ir_remove_t *r = &ir->removes[ir->remove_count++];
            memset(r, 0, sizeof *r);
            ir_parse_remove(&c, t, r);
        } else if (RP_EQ(t->kind, "copy")) {
            rp_ir_copy_t *cp = &ir->copies[ir->copy_count++];
            memset(cp, 0, sizeof *cp);
            ir_parse_copy(&c, t, cp);
        } else if (RP_EQ(t->kind, "env")) {
            rp_ir_env_t *e = &ir->envs[ir->env_count++];
            memset(e, 0, sizeof *e);
            ir_parse_env(&c, t, e);
        } else if (RP_EQ(t->kind, "ini")) {
            rp_ir_ini_t *x = &ir->inis[ir->ini_count++];
            memset(x, 0, sizeof *x);
            ir_parse_ini(&c, t, x);
        } else if (RP_EQ(t->kind, "require")) {
            rp_ir_require_t *r = &ir->requires[ir->require_count++];
            memset(r, 0, sizeof *r);
            ir_parse_require(&c, t, r);
        } else if (RP_EQ(t->kind, "search")) {
            rp_ir_search_t *x = &ir->searches[ir->search_count++];
            memset(x, 0, sizeof *x);
            ir_parse_search(&c, t, x);
        } else if (RP_EQ(t->kind, "service")) {
            rp_ir_service_t *x = &ir->services[ir->service_count++];
            memset(x, 0, sizeof *x);
            ir_parse_service(&c, t, x);
        } else if (RP_EQ(t->kind, "merge")) {
            rp_ir_merge_t *x = &ir->merges[ir->merge_count++];
            memset(x, 0, sizeof *x);
            ir_parse_merge(&c, t, x);
        } else if (RP_EQ(t->kind, "font")) {
            rp_ir_font_t *x = &ir->fonts[ir->font_count++];
            memset(x, 0, sizeof *x);
            ir_parse_font(&c, t, x);
        } else if (RP_EQ(t->kind, "assoc")) {
            rp_ir_assoc_t *x = &ir->assocs[ir->assoc_count++];
            memset(x, 0, sizeof *x);
            ir_parse_assoc(&c, t, x);
        } else if (RP_EQ(t->kind, "protocol")) {
            rp_ir_protocol_t *x = &ir->protocols[ir->protocol_count++];
            memset(x, 0, sizeof *x);
            ir_parse_protocol(&c, t, x);
        } else if (RP_EQ(t->kind, "menu")) {
            rp_ir_menu_t *x = &ir->menus[ir->menu_count++];
            memset(x, 0, sizeof *x);
            ir_parse_menu(&c, t, x);
        } else if (RP_EQ(t->kind, "other-copy")) {
            rp_ir_other_t *x = &ir->others[ir->other_count++];
            memset(x, 0, sizeof *x);
            ir_parse_other(&c, t, x);
        } else if (RP_EQ(t->kind, "move")) {
            rp_ir_move_t *x = &ir->moves[ir->move_count++];
            memset(x, 0, sizeof *x);
            ir_parse_move(&c, t, x);
        } else if (RP_EQ(t->kind, "odbc-driver")) {
            rp_ir_odbc_driver_t *x = &ir->odbc_drivers[ir->odbc_driver_count++];
            memset(x, 0, sizeof *x);
            ir_parse_odbc_driver(&c, t, x);
        } else if (RP_EQ(t->kind, "odbc-source")) {
            rp_ir_odbc_source_t *x = &ir->odbc_sources[ir->odbc_source_count++];
            memset(x, 0, sizeof *x);
            ir_parse_odbc_source(&c, t, x);
        } else if (RP_EQ(t->kind, "publish")) {
            rp_ir_publish_t *x = &ir->publishes[ir->publish_count++];
            memset(x, 0, sizeof *x);
            ir_parse_publish(&c, t, x);
        } else if (RP_EQ(t->kind, "handler")) {
            rp_ir_handler_t *x = &ir->handlers[ir->handler_count++];
            memset(x, 0, sizeof *x);
            ir_parse_handler(&c, t, x);
        } else if (RP_EQ(t->kind, "com")) {
            rp_ir_com_t *x = &ir->coms[ir->com_count++];
            memset(x, 0, sizeof *x);
            ir_parse_com(&c, t, x);
        } else if (RP_EQ(t->kind, "msix-extension")) {
            rp_ir_msix_ext_t *x = &ir->msix_exts[ir->msix_ext_count++];
            memset(x, 0, sizeof *x);
            ir_parse_msix_ext(&c, t, x);
        } else if (RP_EQ(t->kind, "permission")) {
            rp_ir_permission_t *x = &ir->permissions[ir->permission_count++];
            memset(x, 0, sizeof *x);
            ir_parse_permission(&c, t, x);
        } else if (RP_EQ(t->kind, "ui-text")) {
            rp_ir_ui_text_t *x = &ir->ui_texts[ir->ui_text_count++];
            memset(x, 0, sizeof *x);
            ir_parse_ui_text(&c, t, x);
        } else if (RP_EQ(t->kind, "dialog")) {
            rp_ir_dialog_t *x = &ir->dialogs[ir->dialog_count++];
            memset(x, 0, sizeof *x);
            ir_parse_dialog(&c, t, x);
        } else if (RP_EQ(t->kind, "dialog-control")) {
            rp_ir_dialog_control_t *x = &ir->dialog_controls[ir->dialog_control_count++];
            memset(x, 0, sizeof *x);
            ir_parse_dialog_control(&c, t, x);
        } else if (RP_EQ(t->kind, "billboard")) {
            rp_ir_billboard_t *x = &ir->billboards[ir->billboard_count++];
            memset(x, 0, sizeof *x);
            ir_parse_billboard(&c, t, x);
        }
    }
    // Wildcards after every dir is known (their feature and the implicit sub folders).
    for (size_t k = 0; k < doc->count && !c.nomem; ++k) {
        const rp_ttable_t *t = &doc->tables[k];
        if (t->id.ptr && RP_EQ(t->kind, "files")) ir_expand_files(&c, t);
    }
    if (!c.nomem) ir_menu_files(&c);
    if (!c.nomem && ir->feature_count == 0) {       // G2: one hidden default feature
        rp_ir_feature_t *f = &ir->features[ir->feature_count++];
        memset(f, 0, sizeof *f);
        f->id = RP_V(ir_dup(&c, "Main"));
        f->title = RP_V(ir_dup(&c, ir->name.ptr ? rp_cs(ir->name) : "Main"));
        f->level = 1;
        f->hidden = true;
        f->implicit = true;
    }
    // Properties and actions in ID order: the output never depends on the order of tables.
    SORT_BY_ID(ir->properties, ir->property_count, rp_ir_property_t);
    SORT_BY_ID(ir->actions, ir->action_count, rp_ir_action_t);
    SORT_BY_ID(ir->permissions, ir->permission_count, rp_ir_permission_t);
    SORT_BY_ID(ir->fonts, ir->font_count, rp_ir_font_t);
    SORT_BY_ID(ir->services, ir->service_count, rp_ir_service_t);
    SORT_BY_ID(ir->requires, ir->require_count, rp_ir_require_t);
    SORT_BY_ID(ir->searches, ir->search_count, rp_ir_search_t);
    SORT_BY_ID(ir->inis, ir->ini_count, rp_ir_ini_t);
    SORT_BY_ID(ir->envs, ir->env_count, rp_ir_env_t);
    SORT_BY_ID(ir->removes, ir->remove_count, rp_ir_remove_t);
    SORT_BY_ID(ir->copies, ir->copy_count, rp_ir_copy_t);
    SORT_BY_ID(ir->shortcuts, ir->shortcut_count, rp_ir_shortcut_t);
    SORT_BY_ID(ir->assocs, ir->assoc_count, rp_ir_assoc_t);
    SORT_BY_ID(ir->protocols, ir->protocol_count, rp_ir_protocol_t);
    SORT_BY_ID(ir->coms, ir->com_count, rp_ir_com_t);
    SORT_BY_ID(ir->handlers, ir->handler_count, rp_ir_handler_t);
    SORT_BY_ID(ir->menus, ir->menu_count, rp_ir_menu_t);
    SORT_BY_ID(ir->msix_exts, ir->msix_ext_count, rp_ir_msix_ext_t);
    SORT_BY_ID(ir->registries, ir->registry_count, rp_ir_registry_t);
    if (!c.nomem && package) ir_ui_checks(&c, uit, package);
    SORT_BY_ID(ir->ui_texts, ir->ui_text_count, rp_ir_ui_text_t);
    SORT_BY_ID(ir->dialogs, ir->dialog_count, rp_ir_dialog_t);
    SORT_BY_ID(ir->dialog_controls, ir->dialog_control_count, rp_ir_dialog_control_t);
    SORT_BY_ID(ir->billboards, ir->billboard_count, rp_ir_billboard_t);
    if (!c.nomem) ir_index_dirs(&c);
    if (!c.nomem && opt->nfc) ir_nfc_names(&c);
    if (!c.nomem) ir_dialog_checks(&c);
    if (!c.nomem) ir_class_checks(&c);
    if (!c.nomem) ir_cross_checks(&c);
    if (!c.nomem && ir->module) {
        // Every key of a module carries ".<GUID>" (37 characters) within the 72 a key may have;
        // what needs rubrapack's helper DLL stays with packages.
        for (size_t k = 0; k < doc->count; ++k) {
            const rp_ttable_t *t = &doc->tables[k];
            if (t->id.ptr && t->id.size > 35 && !RP_EQ(t->kind, "define")) {
                ERR(&c, t->pos, "RP1301", "[%s.%s]: an ID in a merge module has at most 35 characters (the module's GUID is added to it)",
                    rp_cs(t->kind), rp_cs(t->id));
            }
        }
        for (size_t k = 0; k < ir->dir_count; ++k) {
            if (ir->dirs[k].guard) ERR(&c, ir->dirs[k].pos, "RP1316", "[dir.%s]: guard needs rubrapack's helper DLL, which a merge module does not carry", rp_cs(ir->dirs[k].id));
        }
        for (size_t k = 0; k < ir->remove_count; ++k) {
            if (ir->removes[k].recursive) ERR(&c, ir->removes[k].pos, "RP1316", "[remove.%s]: recursive = true needs rubrapack's helper DLL, which a merge module does not carry", rp_cs(ir->removes[k].id));
        }
        for (size_t k = 0; k < ir->registry_count; ++k) {
            if (ir->registries[k].purge) ERR(&c, ir->registries[k].pos, "RP1316", "[registry.%s]: on = \"uninstall\" needs rubrapack's helper DLL, which a merge module does not carry", rp_cs(ir->registries[k].id));
            if (ir->registries[k].type == RP_REG_QWORD) {
                ERR(&c, ir->registries[k].pos, "RP1316", "[registry.%s]: a qword value needs rubrapack's helper DLL, which a merge module does not carry", rp_cs(ir->registries[k].id));
            }
        }
    }
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
        rp_mem_free(a, (void *)(x[i].lang).ptr);
        rp_mem_free(a, (void *)(x[i].text).ptr);
    }
    rp_mem_free(a, x);
}

void rp_ir_free(rp_ir_t *ir) {
    if (ir == NULL) return;
    proven_allocator_t a = ir->alloc;
    proven_u8str_view_t strs[] = { ir->name, ir->summary_name, ir->manufacturer, ir->version, ir->upgrade_code, ir->product_code,
                     ir->downgrade_message, ir->refuse_below, ir->refuse_message, ir->parent};
    for (size_t k = 0; k < sizeof strs / sizeof strs[0]; ++k) rp_mem_free(a, (void *)strs[k].ptr);
    for (size_t k = 0; k < ir->feature_count; ++k) {
        rp_ir_feature_t *f = &ir->features[k];
        rp_mem_free(a, (void *)(f->id).ptr);
        rp_mem_free(a, (void *)(f->title).ptr);
        rp_mem_free(a, (void *)(f->description).ptr);
        rp_mem_free(a, (void *)(f->parent).ptr);
        rp_mem_free(a, (void *)(f->when).ptr);
        rp_mem_free(a, (void *)(f->default_when).ptr);
    }
    for (size_t k = 0; k < ir->dir_count; ++k) {
        rp_ir_dir_t *d = &ir->dirs[k];
        rp_mem_free(a, (void *)(d->id).ptr);
        rp_mem_free(a, (void *)(d->base).ptr);
        rp_mem_free(a, (void *)(d->parent).ptr);
        rp_mem_free(a, (void *)(d->feature).ptr);
        for (size_t j = 0; j < d->part_count; ++j) rp_mem_free(a, d->parts[j]);
        rp_mem_free(a, d->parts);
    }
    for (size_t k = 0; k < ir->file_count; ++k) {
        rp_ir_file_t *f = &ir->files[k];
        proven_u8str_view_t fs[] = { f->id, f->dir, f->source, f->source_path, f->name, f->feature, f->component_guid, f->when, f->isolate_for,
                       f->assembly_for, f->manifest, f->manifest_of};
        for (size_t j = 0; j < sizeof fs / sizeof fs[0]; ++j) rp_mem_free(a, (void *)fs[j].ptr);
        for (size_t j = 0; j < 6; ++j) rp_mem_free(a, f->asm_names[j][0]), rp_mem_free(a, f->asm_names[j][1]);
        rp_mem_free(a, f->data);
    }
    for (size_t k = 0; k < ir->folder_count; ++k) {
        rp_ir_folder_t *f = &ir->folders[k];
        rp_mem_free(a, (void *)(f->id).ptr);
        rp_mem_free(a, (void *)(f->dir).ptr);
        rp_mem_free(a, (void *)(f->name).ptr);
        rp_mem_free(a, (void *)(f->feature).ptr);
    }
    for (size_t k = 0; k < ir->property_count; ++k) {
        rp_mem_free(a, (void *)(ir->properties[k].id).ptr);
        rp_mem_free(a, (void *)(ir->properties[k].value).ptr);
    }
    for (size_t k = 0; k < ir->action_count; ++k) {
        rp_ir_action_t *x = &ir->actions[k];
        proven_u8str_view_t xs[] = { x->id, x->run_file, x->do_args, x->undo_args, x->check_args, x->args, x->dll_file, x->binary_shown, x->binary_source, x->entry, x->when, x->data, x->before, x->after};
        for (size_t j = 0; j < sizeof xs / sizeof xs[0]; ++j) rp_mem_free(a, (void *)xs[j].ptr);
    }
    for (size_t k = 0; k < ir->registry_count; ++k) {
        rp_ir_registry_t *r = &ir->registries[k];
        proven_u8str_view_t rs[] = { r->id, r->key, r->name, r->value, r->with_file, r->feature, r->when};
        for (size_t j = 0; j < sizeof rs / sizeof rs[0]; ++j) rp_mem_free(a, (void *)rs[j].ptr);
        for (size_t j = 0; j < r->item_count; ++j) rp_mem_free(a, r->items[j]);
        rp_mem_free(a, r->items);
    }
    rp_mem_free(a, ir->registries);
    for (size_t k = 0; k < ir->shortcut_count; ++k) {
        rp_ir_shortcut_t *sc = &ir->shortcuts[k];
        proven_u8str_view_t ss[] = { sc->id, sc->dir, sc->name, sc->target_file, sc->args, sc->description, sc->working_dir, sc->icon_source,
                       sc->icon_shown, sc->when};
        for (size_t j = 0; j < sizeof ss / sizeof ss[0]; ++j) rp_mem_free(a, (void *)ss[j].ptr);
    }
    rp_mem_free(a, ir->shortcuts);
    for (size_t k = 0; k < ir->remove_count; ++k) {
        rp_ir_remove_t *r = &ir->removes[k];
        proven_u8str_view_t xs[] = { r->id, r->dir, r->name, r->feature};
        for (size_t j = 0; j < sizeof xs / sizeof xs[0]; ++j) rp_mem_free(a, (void *)xs[j].ptr);
    }
    for (size_t k = 0; k < ir->copy_count; ++k) {
        rp_ir_copy_t *cp = &ir->copies[k];
        proven_u8str_view_t xs[] = { cp->id, cp->source_file, cp->dir, cp->name};
        for (size_t j = 0; j < sizeof xs / sizeof xs[0]; ++j) rp_mem_free(a, (void *)xs[j].ptr);
    }
    for (size_t k = 0; k < ir->env_count; ++k) {
        rp_ir_env_t *e = &ir->envs[k];
        proven_u8str_view_t xs[] = { e->id, e->name, e->value, e->feature, e->when};
        for (size_t j = 0; j < sizeof xs / sizeof xs[0]; ++j) rp_mem_free(a, (void *)xs[j].ptr);
    }
    rp_mem_free(a, ir->envs);
    for (size_t k = 0; k < ir->ini_count; ++k) {
        rp_ir_ini_t *x = &ir->inis[k];
        proven_u8str_view_t xs[] = { x->id, x->dir, x->file, x->section, x->key, x->value, x->feature, x->when};
        for (size_t j = 0; j < sizeof xs / sizeof xs[0]; ++j) rp_mem_free(a, (void *)xs[j].ptr);
    }
    rp_mem_free(a, ir->inis);
    for (size_t k = 0; k < ir->require_count; ++k) {
        rp_mem_free(a, (void *)(ir->requires[k].id).ptr);
        rp_mem_free(a, (void *)(ir->requires[k].condition).ptr);
        rp_mem_free(a, (void *)(ir->requires[k].message).ptr);
    }
    rp_mem_free(a, ir->requires);
    for (size_t k = 0; k < ir->search_count; ++k) {
        rp_ir_search_t *x = &ir->searches[k];
        proven_u8str_view_t xs[] = { x->id, x->property, x->key, x->name, x->base, x->path, x->file_name, x->min_version, x->component_guid};
        for (size_t j = 0; j < sizeof xs / sizeof xs[0]; ++j) rp_mem_free(a, (void *)xs[j].ptr);
    }
    rp_mem_free(a, ir->searches);
    for (size_t k = 0; k < ir->service_count; ++k) {
        rp_ir_service_t *x = &ir->services[k];
        proven_u8str_view_t xs[] = { x->id, x->file, x->name, x->display_name, x->description, x->args};
        for (size_t j = 0; j < sizeof xs / sizeof xs[0]; ++j) rp_mem_free(a, (void *)xs[j].ptr);
    }
    rp_mem_free(a, ir->services);
    for (size_t k = 0; k < ir->merge_count; ++k) {
        rp_ir_merge_t *x = &ir->merges[k];
        proven_u8str_view_t xs[] = { x->id, x->source, x->shown, x->dir, x->feature};
        for (size_t j = 0; j < sizeof xs / sizeof xs[0]; ++j) rp_mem_free(a, (void *)xs[j].ptr);
        for (size_t j = 0; j < x->config_count; ++j) rp_mem_free(a, x->config[j]);
        rp_mem_free(a, x->config);
    }
    rp_mem_free(a, ir->merges);
    for (size_t k = 0; k < ir->font_count; ++k) {
        rp_mem_free(a, (void *)(ir->fonts[k].id).ptr);
        rp_mem_free(a, (void *)(ir->fonts[k].file).ptr);
        rp_mem_free(a, (void *)(ir->fonts[k].title).ptr);
    }
    rp_mem_free(a, ir->fonts);
    for (size_t k = 0; k < ir->assoc_count; ++k) {
        rp_ir_assoc_t *x = &ir->assocs[k];
        proven_u8str_view_t xs[] = { x->id, x->extension, x->prog_id, x->description, x->target_file, x->icon_file, x->logo, x->logo_path, x->args,
                       x->content_type, x->perceived_type};
        for (size_t j = 0; j < sizeof xs / sizeof xs[0]; ++j) rp_mem_free(a, (void *)xs[j].ptr);
    }
    rp_mem_free(a, ir->assocs);
    for (size_t k = 0; k < ir->protocol_count; ++k) {
        rp_ir_protocol_t *x = &ir->protocols[k];
        proven_u8str_view_t xs[] = { x->id, x->name, x->description, x->target_file, x->args};
        for (size_t j = 0; j < sizeof xs / sizeof xs[0]; ++j) rp_mem_free(a, (void *)xs[j].ptr);
    }
    rp_mem_free(a, ir->protocols);
    for (size_t k = 0; k < ir->com_count; ++k) {
        rp_ir_com_t *x = &ir->coms[k];
        proven_u8str_view_t xs[] = { x->id, x->file, x->clsid, x->description, x->threading, x->args, x->prog_id, x->app_id, x->typelib,
                       x->typelib_version, x->typelib_file};
        for (size_t j = 0; j < sizeof xs / sizeof xs[0]; ++j) rp_mem_free(a, (void *)xs[j].ptr);
    }
    rp_mem_free(a, ir->coms);
    for (size_t k = 0; k < ir->handler_count; ++k) {
        rp_ir_handler_t *x = &ir->handlers[k];
        rp_mem_free(a, (void *)(x->id).ptr);
        rp_mem_free(a, (void *)(x->clsid).ptr);
        rp_mem_free(a, (void *)(x->description).ptr);
        for (size_t j = 0; j < x->type_count; ++j) rp_mem_free(a, x->types[j]);
        rp_mem_free(a, x->types);
    }
    rp_mem_free(a, ir->handlers);
    for (size_t k = 0; k < ir->menu_count; ++k) {
        rp_ir_menu_t *x = &ir->menus[k];
        proven_u8str_view_t xs[] = { x->id, x->text, x->target_file, x->icon_file, x->args, x->list, x->hide_when, x->parent, x->clsid};
        for (size_t j = 0; j < sizeof xs / sizeof xs[0]; ++j) rp_mem_free(a, (void *)xs[j].ptr);
        for (size_t j = 0; j < x->on_count; ++j) rp_mem_free(a, x->on[j]);
        rp_mem_free(a, x->on);
        for (size_t j = 0; j < x->text_by_lang_count; ++j) {
            rp_mem_free(a, (void *)(x->text_by_lang[j].lang).ptr);
            rp_mem_free(a, (void *)(x->text_by_lang[j].text).ptr);
        }
        rp_mem_free(a, x->text_by_lang);
    }
    rp_mem_free(a, ir->menus);
    for (size_t k = 0; k < ir->other_count; ++k) {
        rp_ir_other_t *x = &ir->others[k];
        proven_u8str_view_t xs[] = { x->id, x->msix, x->publisher, x->upgrade_code, x->message};
        for (size_t j = 0; j < sizeof xs / sizeof xs[0]; ++j) rp_mem_free(a, (void *)xs[j].ptr);
        for (size_t j = 0; j < x->message_by_lang_count; ++j) {
            rp_mem_free(a, (void *)(x->message_by_lang[j].lang).ptr);
            rp_mem_free(a, (void *)(x->message_by_lang[j].text).ptr);
        }
        rp_mem_free(a, x->message_by_lang);
    }
    rp_mem_free(a, ir->others);
    ir_more_free(ir, a);
    rp_mem_free(a, (void *)(ir->menu_exe).ptr);
    rp_mem_free(a, (void *)(ir->menu_publisher).ptr);
    for (size_t k = 0; k < ir->msix_ext_count; ++k) {
        rp_ir_msix_ext_t *x = &ir->msix_exts[k];
        proven_u8str_view_t xs[] = { x->id, x->app, x->alias, x->task_id, x->display, x->file, x->profile, x->clsid, x->threading, x->args, x->verb,
                       x->prog_id, x->name, x->ext_id, x->folder, x->description, x->entry, x->action};
        for (size_t j = 0; j < sizeof xs / sizeof xs[0]; ++j) rp_mem_free(a, (void *)xs[j].ptr);
        for (size_t j = 0; j < x->type_count; ++j) rp_mem_free(a, x->types[j]);
        rp_mem_free(a, x->types);
        for (size_t j = 0; j < x->name_count; ++j) rp_mem_free(a, x->names[j]);
        rp_mem_free(a, x->names);
        for (size_t j = 0; j < x->event_count; ++j) rp_mem_free(a, x->events[j]);
        rp_mem_free(a, x->events);
    }
    rp_mem_free(a, ir->msix_exts);
    for (size_t k = 0; k < ir->permission_count; ++k) {
        rp_ir_permission_t *x = &ir->permissions[k];
        proven_u8str_view_t xs[] = { x->id, x->target, x->sddl, x->feature};
        for (size_t j = 0; j < sizeof xs / sizeof xs[0]; ++j) rp_mem_free(a, (void *)xs[j].ptr);
    }
    rp_mem_free(a, ir->permissions);
    for (size_t k = 0; k < ir->ui_text_count; ++k) {
        rp_mem_free(a, (void *)(ir->ui_texts[k].id).ptr);
        rp_mem_free(a, (void *)(ir->ui_texts[k].text).ptr);
        free_ltexts(a, ir->ui_texts[k].by_lang, ir->ui_texts[k].by_lang_count);
    }
    rp_mem_free(a, ir->ui_texts);
    for (size_t k = 0; k < ir->ui_lang_count; ++k) {
        proven_u8str_view_t xs[] = { ir->ui_langs[k].name, ir->ui_langs[k].font, ir->ui_langs[k].license_source, ir->ui_langs[k].license_shown};
        for (size_t j = 0; j < sizeof xs / sizeof xs[0]; ++j) rp_mem_free(a, (void *)xs[j].ptr);
    }
    for (size_t k = 0; k < ir->dialog_count; ++k) {
        proven_u8str_view_t xs[] = { ir->dialogs[k].id, ir->dialogs[k].title, ir->dialogs[k].description, ir->dialogs[k].after};
        for (size_t j = 0; j < sizeof xs / sizeof xs[0]; ++j) rp_mem_free(a, (void *)xs[j].ptr);
        free_ltexts(a, ir->dialogs[k].title_by_lang, ir->dialogs[k].title_by_lang_count);
        free_ltexts(a, ir->dialogs[k].description_by_lang, ir->dialogs[k].description_by_lang_count);
    }
    rp_mem_free(a, ir->dialogs);
    for (size_t k = 0; k < ir->dialog_control_count; ++k) {
        rp_ir_dialog_control_t *x = &ir->dialog_controls[k];
        proven_u8str_view_t xs[] = { x->id, x->dialog, x->text, x->property, x->image_source, x->image_shown};
        for (size_t j = 0; j < sizeof xs / sizeof xs[0]; ++j) rp_mem_free(a, (void *)xs[j].ptr);
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
            rp_mem_free(a, (void *)(x->labels_by_lang[j].lang).ptr);
        }
        rp_mem_free(a, x->labels_by_lang);
    }
    rp_mem_free(a, ir->dialog_controls);
    for (size_t k = 0; k < ir->billboard_count; ++k) {
        rp_ir_billboard_t *x = &ir->billboards[k];
        proven_u8str_view_t xs[] = { x->id, x->image_source, x->image_shown, x->feature};
        for (size_t j = 0; j < sizeof xs / sizeof xs[0]; ++j) rp_mem_free(a, (void *)xs[j].ptr);
    }
    rp_mem_free(a, ir->billboards);
    proven_u8str_view_t us[] = { ir->license_source, ir->license_shown, ir->banner_source, ir->ui_install_dir, ir->ui_launch_file, ir->ui_launch_args,
                   ir->ui_embedded_source, ir->ui_embedded_shown};
    for (size_t k = 0; k < sizeof us / sizeof us[0]; ++k) rp_mem_free(a, (void *)us[k].ptr);
    rp_mem_free(a, ir->removes);
    rp_mem_free(a, ir->copies);
    rp_mem_free(a, ir->properties);
    rp_mem_free(a, ir->actions);
    rp_mem_free(a, (void *)(ir->arp_help).ptr);
    rp_mem_free(a, (void *)(ir->arp_icon_source).ptr);
    rp_mem_free(a, (void *)(ir->arp_logo_source).ptr);
    rp_mem_free(a, (void *)(ir->arp_logo_shown).ptr);
    rp_mem_free(a, (void *)(ir->arp_icon_shown).ptr);
    rp_mem_free(a, (void *)(ir->arp_about).ptr);
    rp_mem_free(a, (void *)(ir->msix_identity_name).ptr);
    rp_mem_free(a, (void *)(ir->msix_publisher).ptr);
    rp_mem_free(a, (void *)(ir->msix_publisher_display).ptr);
    rp_mem_free(a, (void *)(ir->msix_display).ptr);
    free_ltexts(a, ir->msix_display_by_lang, ir->msix_display_by_lang_count);
    free_ltexts(a, ir->msix_publisher_display_by_lang, ir->msix_publisher_display_by_lang_count);
    rp_mem_free(a, (void *)(ir->msix_min_version).ptr);
    rp_mem_free(a, (void *)(ir->msix_appinstaller_uri).ptr);
    rp_mem_free(a, (void *)(ir->msix_package_uri).ptr);
    for (size_t k = 0; k < ir->msix_app_count; ++k) {
        rp_ir_msix_app_t *x = &ir->msix_apps[k];
        rp_mem_free(a, (void *)(x->id).ptr);
        rp_mem_free(a, (void *)(x->exe).ptr);
        rp_mem_free(a, (void *)(x->display).ptr);
        rp_mem_free(a, (void *)(x->background).ptr);
        rp_mem_free(a, (void *)(x->description).ptr);
        free_ltexts(a, x->display_by_lang, x->display_by_lang_count);
        free_ltexts(a, x->description_by_lang, x->description_by_lang_count);
        for (int i = 0; i < 3; ++i) {
            rp_mem_free(a, x->logo[i]);
            rp_mem_free(a, x->logo_path[i]);
        }
    }
    rp_mem_free(a, ir->msix_apps);
    rp_mem_free(a, (void *)(ir->msix_main).ptr);
    rp_mem_free(a, (void *)(ir->msix_main_publisher).ptr);
    for (size_t k = 0; k < ir->msix_cap_count; ++k) rp_mem_free(a, ir->msix_caps[k]);
    rp_mem_free(a, ir->msix_caps);
    proven_u8str_view_t wg[] = { ir->wg_id, ir->wg_url, ir->wg_license, ir->wg_desc, ir->wg_publisher_url, ir->wg_package_url, ir->wg_license_url, ir->wg_moniker};
    for (size_t k = 0; k < sizeof wg / sizeof wg[0]; ++k) rp_mem_free(a, (void *)wg[k].ptr);
    for (size_t k = 0; k < ir->wg_tag_count; ++k) rp_mem_free(a, ir->wg_tags[k]);
    rp_mem_free(a, ir->wg_tags);
    free_ltexts(a, ir->wg_desc_by_lang, ir->wg_desc_by_lang_count);
    rp_mem_free(a, (void *)(ir->psf_files).ptr);
    rp_mem_free(a, (void *)(ir->psf_config).ptr);
    rp_mem_free(a, (void *)(ir->psf_launcher).ptr);
    for (size_t k = 0; k < ir->msix_symbol_count; ++k) rp_mem_free(a, ir->msix_symbols[k]);
    rp_mem_free(a, ir->msix_symbols);
    for (size_t k = 0; k < ir->msix_dep_count; ++k) {
        rp_ir_msix_dep_t *x = &ir->msix_deps[k];
        proven_u8str_view_t xs[] = { x->id, x->name, x->publisher, x->min_version};
        for (size_t j = 0; j < sizeof xs / sizeof xs[0]; ++j) rp_mem_free(a, (void *)xs[j].ptr);
    }
    rp_mem_free(a, ir->msix_deps);
    for (size_t i = 0; i < ir->msix_block_count; ++i) {
        rp_mem_free(a, (void *)(ir->msix_blocks[i].kind).ptr);
        rp_mem_free(a, (void *)(ir->msix_blocks[i].id).ptr);
    }
    rp_mem_free(a, ir->msix_blocks);
    rp_mem_free(a, ir->folders);
    rp_mem_free(a, ir->features);
    rp_mem_free(a, ir->dirs);
    rp_mem_free(a, ir->dir_order);
    rp_mem_free(a, ir->files);
    memset(ir, 0, sizeof *ir);
    ir->alloc = a;
}
