// src/model/ir_check.c - checks across tables.

#include "ir_int.h"

// ---- cross checks --------------------------------------------------------------------------

const rp_ir_dir_t *rp_ir_find_dir(const rp_ir_t *ir, const char *id) {
    if (ir->dir_order && ir->dir_order_of == ir->dir_count) {      // the index is of these dirs
        size_t lo = 0, hi = ir->dir_order_len;                      // the first of equal IDs
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            if (RP_CMP(ir->dirs[ir->dir_order[mid]].id, id) < 0) lo = mid + 1;
            else hi = mid;
        }
        if (lo < ir->dir_order_len && RP_EQ(ir->dirs[ir->dir_order[lo]].id, id)) return &ir->dirs[ir->dir_order[lo]];
        return NULL;
    }
    for (size_t k = 0; k < ir->dir_count; ++k) {
        if (ir->dirs[k].id.ptr && RP_EQ(ir->dirs[k].id, id)) return &ir->dirs[k];
    }
    return NULL;
}

typedef struct { const char *id; size_t at; } dirat_t;

static int cmp_dirat(const void *x, const void *y) {
    const dirat_t *a = x, *b = y;
    int r = strcmp(a->id, b->id);
    return r ? r : a->at < b->at ? -1 : a->at > b->at;
}

// rp_ir_find_dir's index, once every dir is known: the dirs with an ID by ID, then by place.
void ir_index_dirs(ctx_t *c) {
    rp_ir_t *ir = c->ir;
    rp_mem_free(c->alloc, ir->dir_order);
    ir->dir_order = NULL;
    ir->dir_order_len = ir->dir_order_of = 0;
    dirat_t *v = rp_mem_alloc(c->alloc, ir->dir_count ? ir->dir_count : 1, sizeof *v);
    size_t *order = rp_mem_alloc(c->alloc, ir->dir_count ? ir->dir_count : 1, sizeof *order);
    if (v == NULL || order == NULL) {
        rp_mem_free(c->alloc, v);
        rp_mem_free(c->alloc, order);
        c->nomem = true;
        return;
    }
    size_t n = 0;
    for (size_t k = 0; k < ir->dir_count; ++k) {
        if (ir->dirs[k].id.ptr) v[n++] = (dirat_t){ rp_cs(ir->dirs[k].id), k };
    }
    rp_sort(v, n, sizeof *v, cmp_dirat);
    for (size_t k = 0; k < n; ++k) order[k] = v[k].at;
    rp_mem_free(c->alloc, v);
    ir->dir_order = order;
    ir->dir_order_len = n;
    ir->dir_order_of = ir->dir_count;
}

static const rp_ir_feature_t *find_feature(const rp_ir_t *ir, const char *id) {
    for (size_t k = 0; k < ir->feature_count; ++k) {
        if (ir->features[k].id.ptr && RP_EQ(ir->features[k].id, id)) return &ir->features[k];
    }
    return NULL;
}

// Full target path of a dir (known folder / parts...), or NULL when the chain is broken or loops.
static char *dir_full_path(ctx_t *c, const rp_ir_dir_t *d, size_t depth) {
    if (depth > c->ir->dir_count || d->parts == NULL) return NULL;
    char *head;
    if (d->base.ptr) {
        head = ir_dup(c, rp_cs(d->base));
    } else {
        const rp_ir_dir_t *p = d->parent.ptr ? rp_ir_find_dir(c->ir, rp_cs(d->parent)) : NULL;
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

int ir_cmp_str(const char *a, const char *b) { return strcmp(a ? a : "", b ? b : ""); }

static const rp_ir_file_t *file_by_id(const rp_ir_t *ir, const char *id) {
    for (size_t j = 0; id && j < ir->file_count; ++j) {
        if (RP_EQ(ir->files[j].id, id)) return &ir->files[j];
    }
    return NULL;
}

// A program of this package that opens files or URIs: a [file.*] whose name ends in .exe.
// The dir `id` is a folder of the user's profile or below one (lower.c's dir_in_profile, RFC-0029);
// `data_only`: AppData and LocalAppData only, the folders that stay the user's in a per-machine
// installation (the Start menu and the desktop become the all-users ones).
static bool profile_base(const rp_ir_t *ir, const char *id, bool data_only) {
    static const char *const profile[] = { "AppData", "LocalAppData", "StartMenu", "Programs", "Desktop", "Startup" };
    for (size_t depth = 0; id && depth <= ir->dir_count; ++depth) {
        const rp_ir_dir_t *d = rp_ir_find_dir(ir, id);
        if (d == NULL || d->base.ptr) {
            const char *base = d ? rp_cs(d->base) : id;
            for (size_t i = 0; i < (data_only ? 2u : sizeof profile / sizeof profile[0]); ++i) {
                if (strcmp(base, profile[i]) == 0) return true;
            }
            return false;
        }
        id = rp_cs(d->parent);
    }
    return false;
}

static bool program_ok(ctx_t *c, const char *id, rp_pos_t pos, const char *what) {
    if (id == NULL) return false;
    const rp_ir_file_t *f = file_by_id(c->ir, id);
    if (f == NULL) {
        ERR(c, pos, "RP1315", "%s: file '%s' is not a [file.*] of this package", what, id);
        return false;
    }
    const char *n = f->name.ptr ? rp_cs(f->name) : "";
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
    r->id = RP_V(ir_dup(c, rid));
    r->root = RP_ROOT_HKCR;
    r->key = RP_V(ir_dup(c, key));
    r->name = RP_V(name ? ir_dup(c, name) : NULL);
    r->type = RP_REG_STRING;
    r->value = RP_V(ir_dup(c, value));
    r->msi_only = true;             // an MSIX says the same in its manifest (RFC-0010 N4)
    r->with_file = RP_V(ir_dup(c, with));
    r->pos = pos;
}

// The same under another root (RFC-0024: HKLM / HKMU values of an Explorer handler).
static void add_root_value(ctx_t *c, const char *id, const char *suffix, rp_reg_root_t root, const char *key, const char *name,
                           const char *value, const char *with, rp_pos_t pos) {
    add_class_value(c, id, suffix, key, name, value, with, pos);
    c->ir->registries[c->ir->registry_count - 1].root = root;
}

static const rp_ir_com_t *com_by_class(const rp_ir_t *ir, const char *clsid) {
    for (size_t k = 0; clsid && k < ir->com_count; ++k) {
        if (ir->coms[k].clsid.ptr && RP_EQ(ir->coms[k].clsid, clsid)) return &ir->coms[k];
    }
    return NULL;
}

// [assoc.*], [protocol.*] and [msix-extension.*] (RFC-0010 N4): programs that exist, one handler per
// extension and scheme, one description per prog-id; then, for the MSI, their HKCR values in the
// program's component. HKCR follows the installation: HKLM\Software\Classes per machine,
// HKCU\Software\Classes per user.
// ---- [menu.*] --------------------------------------------------------------------------------

static const rp_ir_menu_t *menu_by_id(const rp_ir_t *ir, const char *id) {
    for (size_t k = 0; id && k < ir->menu_count; ++k) {
        if (RP_EQ(ir->menus[k].id, id)) return &ir->menus[k];
    }
    return NULL;
}

// The package's name as a registry key part: its letters and digits (a verb's key is
// "<that>.<menu ID>", so two products' items do not meet in one key).
void rp_menu_token(const rp_ir_t *ir, char out[48]) {
    size_t w = 0;
    for (const char *p = ir->name.ptr ? rp_cs(ir->name) : ""; *p && w < 40; ++p) {
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9')) out[w++] = *p;
    }
    if (w == 0) out[w++] = 'R', out[w++] = 'p';
    out[w] = '\0';
}

// The Windows 11 menu of an MSI: whether the package wants it, beside which program its two files
// go, and - when the build command has made them (rp_ir_options_t) - the files themselves. Called
// when every [file.*] and [menu.*] is known, before features and files are settled.
void ir_menu_files(ctx_t *c) {
    rp_ir_t *ir = c->ir;
    if (ir->module) return;
    // The first top-level item by ID with a program (a sub-menu: its first item's).
    const rp_ir_menu_t *first = NULL;
    for (size_t k = 0; k < ir->menu_count; ++k) {
        const rp_ir_menu_t *x = &ir->menus[k];
        if (x->parent.ptr || !x->windows11 || x->id.ptr == NULL) continue;
        if (first == NULL || ir_cmp_str(rp_cs(x->id), rp_cs(first->id)) < 0) first = x;
    }
    if (first == NULL) return;
    const char *exe = rp_cs(first->target_file);
    const rp_ir_menu_t *kid = NULL;
    for (size_t k = 0; exe == NULL && k < ir->menu_count; ++k) {
        const rp_ir_menu_t *y = &ir->menus[k];
        if (y->parent.ptr && y->id.ptr && y->target_file.ptr && RP_EQ(y->parent, first->id) && (kid == NULL || ir_cmp_str(rp_cs(y->id), rp_cs(kid->id)) < 0)) kid = y;
    }
    if (exe == NULL && kid) exe = rp_cs(kid->target_file);
    const rp_ir_file_t *prog = exe ? file_by_id(ir, exe) : NULL;
    if (prog == NULL) return;                   // menu_checks says what is wrong
    ir->menu_package = true;
    ir->menu_exe = RP_V(ir_dup(c, exe));
    if (c->opt->menu_publisher.ptr) ir->menu_publisher = RP_V(ir_dup(c, rp_cs(c->opt->menu_publisher)));
    if (c->opt->menu_dll.ptr == NULL) return;
    // The DLL goes into an MSI and an MSIX; the identity package is an MSI's alone (an MSIX is one).
    static const char *const ids[] = { "RpMenuDll", "RpMenuPkg" }, *const names[] = { "rubrapack_menu.dll", "rubrapack_menu.msix" };
    const char *paths[] = { rp_cs(c->opt->menu_dll), rp_cs(c->opt->menu_pkg)};
    // ir_push_file may move the files: what is needed of the program first.
    char *dir = ir_dup(c, rp_cs(prog->dir)), *feature = prog->feature.ptr ? ir_dup(c, rp_cs(prog->feature)) : NULL;
    rp_pos_t pos = first->pos;
    for (int i = 0; i < 2 && dir; ++i) {
        if (paths[i] == NULL) continue;
        rp_ir_file_t *f = ir_push_file(c);
        if (f == NULL) break;
        f->id = RP_V(ir_dup(c, ids[i]));
        f->dir = RP_V(ir_dup(c, dir));
        f->source = RP_V(ir_dup(c, names[i]));
        f->source_path = RP_V(ir_dup(c, paths[i]));
        f->name = RP_V(ir_dup(c, names[i]));
        f->vital = true;
        f->any_arch = true;
        f->msi_only = i == 1;
        f->feature = RP_V(feature ? ir_dup(c, feature) : NULL);
        f->pos = pos;
        if (rp_pal_stat(c->alloc, rp_cs(f->source_path), &f->size) != RP_FS_FILE) ERR(c, pos, "RP1508", "'%s' is not a regular file", rp_cs(f->source));
    }
    rp_mem_free(c->alloc, dir);
    rp_mem_free(c->alloc, feature);
}

// One value of the menu DLL's data: HKLM per machine, HKCU per user (HKMU), msi-only.
static void add_menu_data(ctx_t *c, const char *id, const char *suffix, const char *key, const char *name, const char *value, int type,
                          const char *with, rp_pos_t pos) {
    rp_ir_registry_t *r = &c->ir->registries[c->ir->registry_count++];
    memset(r, 0, sizeof *r);
    char rid[96];
    snprintf(rid, sizeof rid, "%s.%s", id, suffix);
    r->id = RP_V(ir_dup(c, rid));
    r->root = RP_ROOT_HKMU;
    r->key = RP_V(ir_dup(c, key));
    r->name = RP_V(ir_dup(c, name));
    r->type = (rp_reg_type_t)type;
    r->value = RP_V(ir_dup(c, value));
    r->msi_only = true;
    r->with_file = RP_V(ir_dup(c, with));
    r->pos = pos;
}

static char *fmt_literal(ctx_t *c, const char *s);

// What the menu DLL reads for one item (src/menu/rubrapack_menu.c): Text, Text.<lang>, Icon,
// Command, Args, Multi; a sub-menu: Items.
static void menu_data_at(ctx_t *c, const rp_ir_menu_t *x, const char *key, const char *tag, const char *with) {
    rp_ir_t *ir = c->ir;
    char rid[48], name[16];
    static const char *const multi[] = { "each", "one", "single" };
    if (x->text.ptr) {
        char *t = fmt_literal(c, rp_cs(x->text));
        snprintf(rid, sizeof rid, "%sText", tag);
        if (t) add_menu_data(c, rp_cs(x->id), rid, key, "Text", t, RP_REG_STRING, with, x->pos);
        rp_mem_free(c->alloc, t);
    }
    for (size_t w = 0; w < x->text_by_lang_count; ++w) {
        char *t = fmt_literal(c, rp_cs(x->text_by_lang[w].text));
        // Without an English text the first language's is the fallback too.
        bool en = RP_EQ(x->text_by_lang[w].lang, "en");
        if (t && (en || (x->text.ptr == NULL && w == 0))) {
            snprintf(rid, sizeof rid, "%sText", tag);
            add_menu_data(c, rp_cs(x->id), rid, key, "Text", t, RP_REG_STRING, with, x->pos);
        }
        if (t && !en) {
            snprintf(rid, sizeof rid, "%sText_%s", tag, rp_cs(x->text_by_lang[w].lang));
            snprintf(name, sizeof name, "Text.%s", rp_cs(x->text_by_lang[w].lang));
            add_menu_data(c, rp_cs(x->id), rid, key, name, t, RP_REG_STRING, with, x->pos);
        }
        rp_mem_free(c->alloc, t);
    }
    const char *icon = rp_cs(x->icon_file.ptr ? x->icon_file : x->target_file);
    if (icon) {
        char val[160];
        snprintf(val, sizeof val, x->icon_file.ptr ? "[#%s]" : "[#%s],0", icon);
        snprintf(rid, sizeof rid, "%sIcon", tag);
        add_menu_data(c, rp_cs(x->id), rid, key, "Icon", val, RP_REG_STRING, with, x->pos);
    }
    if (x->hide_when.ptr) {
        char *h = fmt_literal(c, rp_cs(x->hide_when));
        snprintf(rid, sizeof rid, "%sHideWhen", tag);
        if (h) add_menu_data(c, rp_cs(x->id), rid, key, "HideWhen", h, RP_REG_STRING, with, x->pos);
        rp_mem_free(c->alloc, h);
    }
    if (x->target_file.ptr == NULL) {           // a sub-menu: its items' IDs, in ID order
        size_t n = 0;
        for (size_t j = 0; j < ir->menu_count; ++j) n += ir->menus[j].parent.ptr && RP_EQ(ir->menus[j].parent, x->id);
        snprintf(rid, sizeof rid, "%sItems", tag);
        add_menu_data(c, rp_cs(x->id), rid, key, "Items", "", RP_REG_MULTI, with, x->pos);
        rp_ir_registry_t *r = &ir->registries[ir->registry_count - 1];
        r->items = rp_mem_alloc(c->alloc, n ? n : 1, sizeof *r->items);
        if (r->items == NULL) {
            c->nomem = true;
            return;
        }
        for (size_t j = 0; j < ir->menu_count; ++j) {
            if (ir->menus[j].parent.ptr && RP_EQ(ir->menus[j].parent, x->id)) r->items[r->item_count++] = ir_dup(c, rp_cs(ir->menus[j].id));
        }
        return;
    }
    char val[160];
    snprintf(val, sizeof val, "[#%s]", rp_cs(x->target_file));
    snprintf(rid, sizeof rid, "%sCommand", tag);
    add_menu_data(c, rp_cs(x->id), rid, key, "Command", val, RP_REG_STRING, with, x->pos);
    snprintf(rid, sizeof rid, "%sArgs", tag);
    add_menu_data(c, rp_cs(x->id), rid, key, "Args", rp_cs(x->args), RP_REG_STRING, with, x->pos);
    snprintf(rid, sizeof rid, "%sMulti", tag);
    add_menu_data(c, rp_cs(x->id), rid, key, "Multi", multi[x->multi], RP_REG_STRING, with, x->pos);
    if (x->list.ptr) {
        snprintf(rid, sizeof rid, "%sList", tag);
        add_menu_data(c, rp_cs(x->id), rid, key, "List", rp_cs(x->list), RP_REG_STRING, with, x->pos);
    }
}

static void menu_checks(ctx_t *c) {
    rp_ir_t *ir = c->ir;
    for (size_t k = 0; k < ir->menu_count; ++k) {
        rp_ir_menu_t *x = &ir->menus[k];
        const rp_ir_menu_t *up = menu_by_id(ir, rp_cs(x->parent));
        if (x->parent.ptr && up == NULL) ERR(c, x->pos, "RP1307", "[menu.%s]: parent '%s' is not a [menu.*]", rp_cs(x->id), rp_cs(x->parent));
        if (up && (up->target_file.ptr || up->parent.ptr)) {
            ERR(c, x->pos, "RP1316", "[menu.%s]: parent '%s' must be a sub-menu: a [menu.*] without target and without a parent of its own",
                rp_cs(x->id), rp_cs(x->parent));
        }
        if (x->parent.ptr && x->on_count) ERR(c, x->pos, "RP1316", "[menu.%s]: an item of a sub-menu shows where its sub-menu shows: leave 'on' out", rp_cs(x->id));
        if (!x->parent.ptr && x->on_count == 0) ERR(c, x->pos, "RP1202", "[menu.%s] needs 'on': where the item shows, like [\".txt\"] or \"folder\"", rp_cs(x->id));
        // The ID is the verb's Id in a package manifest, which Windows takes as letters and digits only.
        bool alnum = true;
        for (const char *q = rp_cs(x->id); *q; ++q) alnum &= (*q >= 'A' && *q <= 'Z') || (*q >= 'a' && *q <= 'z') || (*q >= '0' && *q <= '9');
        if (!x->parent.ptr && x->windows11 && !alnum) {
            ERR(c, x->pos, "RP1316", "[menu.%s]: Windows 11's menu takes an ID of letters and digits only (no '_'): rename it, or windows11 = false", rp_cs(x->id));
        }
        // list: the paths in a file when they do not fit a command line - for one run with %*.
        if (x->list.ptr && (x->target_file.ptr == NULL || x->multi != RP_MENU_ONE || x->args.ptr == NULL || strstr(rp_cs(x->args), "%*") == NULL ||
                        strstr(rp_cs(x->list), "%L") == NULL)) {
            ERR(c, x->pos, "RP1316", "[menu.%s]: list stands for %s when the paths do not fit a command line: it needs multi = \"one\", %s in args and %s (the list file) in list",
                rp_cs(x->id), "%*", "%*", "%L");
        }
        // hide-when: a registry value whose presence hides the Windows 11 item.
        if (x->hide_when.ptr) {
            const char *h = rp_cs(x->hide_when), *last = strrchr(h, '\\');
            bool root = strncmp(h, "HKCU\\", 5) == 0 || strncmp(h, "HKLM\\", 5) == 0;
            if (!root || last == NULL || last <= h + 5 || last[1] == 0 || strstr(h, "\\\\") || x->parent.ptr || !x->windows11) {
                ERR(c, x->pos, "RP1316", "[menu.%s]: hide-when is \"HKCU\\\\key\\\\value\" or \"HKLM\\\\...\", on an item of Windows 11's menu that is not in a sub-menu", rp_cs(x->id));
            } else {
                // A value this package writes itself would hide the item for good.
                for (size_t r = 0; r < ir->registry_count; ++r) {
                    const rp_ir_registry_t *g = &ir->registries[r];
                    bool cu = g->root == RP_ROOT_HKCU || g->root == RP_ROOT_HKMU, lm = g->root == RP_ROOT_HKLM || g->root == RP_ROOT_HKMU;
                    if (!(h[2] == 'C' ? cu : lm) || g->key.ptr == NULL || g->name.ptr == NULL) continue;
                    char *k = ir_dup(c, h + 5);
                    if (k) k[last - (h + 5)] = 0;
                    bool same = k && ir_ascii_casecmp(rp_cs(g->key), k) == 0 && ir_ascii_casecmp(rp_cs(g->name), last + 1) == 0;
                    rp_mem_free(c->alloc, k);
                    if (same)
                        ERR(c, x->pos, "RP1316", "[menu.%s]: hide-when names a value this package writes ([registry.%s]): the item would never show", rp_cs(x->id), rp_cs(g->id));
                }
            }
        }
        if (x->target_file.ptr) {
            program_ok(c, rp_cs(x->target_file), x->pos, "target");
            if (x->icon_file.ptr && file_by_id(ir, rp_cs(x->icon_file)) == NULL) ERR(c, x->pos, "RP1315", "icon: file '%s' is not a [file.*] of this package", rp_cs(x->icon_file));
            if (x->args.ptr == NULL) x->args = RP_V(ir_dup(c, "\"%1\""));
        } else {
            size_t kids = 0;
            for (size_t j = 0; j < ir->menu_count; ++j) kids += ir->menus[j].parent.ptr && RP_EQ(ir->menus[j].parent, x->id);
            if (x->parent.ptr) ERR(c, x->pos, "RP1202", "[menu.%s] needs 'target': the program the item starts", rp_cs(x->id));
            else if (kids == 0) ERR(c, x->pos, "RP1202", "[menu.%s] needs 'target' (the program it starts), or items with parent = \"%s\" (a sub-menu)", rp_cs(x->id), rp_cs(x->id));
            if (x->args.ptr) ERR(c, x->pos, "RP1316", "[menu.%s]: a sub-menu starts nothing: leave 'args' out", rp_cs(x->id));
        }
        for (size_t j = 0; j < x->on_count; ++j) {
            if (x->on[j] == NULL || strncmp(x->on[j], "assoc:", 6) != 0) continue;
            bool found = false;
            for (size_t a = 0; a < ir->assoc_count; ++a) found |= RP_EQ(ir->assocs[a].id, x->on[j] + 6);
            if (!found) ERR(c, x->pos, "RP1307", "[menu.%s]: on = \"%s\": there is no [assoc.%s]", rp_cs(x->id), x->on[j], x->on[j] + 6);
        }
        // The Windows 11 class of a top-level item: the same in every version of the product.
        if (!x->parent.ptr && ir->upgrade_code.ptr) {
            char guid[39];
            const char *fields[] = { rp_cs(ir->upgrade_code), rp_cs(x->id)};
            rp_uuid_derive("rubrapack.menu", fields, 2, guid);
            x->clsid = RP_V(ir_dup(c, guid));
        }
    }
}

// A literal text as a Formatted value: "[" and "]" written so that they stay.
static char *fmt_literal(ctx_t *c, const char *s) {
    size_t n = strlen(s), extra = 0;
    for (const char *p = s; *p; ++p) extra += (*p == '[' || *p == ']') ? 3 : 0;
    char *o = rp_mem_alloc(c->alloc, n + extra + 1, 1);
    if (o == NULL) {
        c->nomem = true;
        return NULL;
    }
    size_t w = 0;
    for (const char *p = s; *p; ++p) {
        if (*p == '[' || *p == ']') {
            o[w++] = '[';
            o[w++] = '\\';
            o[w++] = *p;
            o[w++] = ']';
        } else {
            o[w++] = *p;
        }
    }
    o[w] = '\0';
    return o;
}

// The classic menu: registry verbs under each place an item shows (MSI; an MSIX's registry is not
// what Explorer reads - there the menu DLL serves both menus).
//   <place>\shell\<Package>.<ID>          MUIVerb, Icon, Extended, MultiSelectModel
//   <place>\shell\<Package>.<ID>\command  "program" args
// A sub-menu has SubCommands = "" and its items under ...\shell\<Package>.<ID>\shell\<item>.
static void menu_values_at(ctx_t *c, const rp_ir_menu_t *x, const char *base, const char *suffix, bool background, const char *with) {
    char key[400], rid[40], prop[64];
    char *text = NULL;
    if (x->text_by_lang_count) {            // the text of the language chosen at installation (lower.c sets the property)
        snprintf(prop, sizeof prop, "[RpMenuT_%s]", rp_cs(x->id));
        text = ir_dup(c, prop);
    } else if (x->text.ptr) {
        text = fmt_literal(c, rp_cs(x->text));
    }
    if (text == NULL) return;
    snprintf(rid, sizeof rid, "Verb%s", suffix);
    add_class_value(c, rp_cs(x->id), rid, base, "MUIVerb", text, with, x->pos);
    rp_mem_free(c->alloc, text);
    const char *icon = rp_cs(x->icon_file.ptr ? x->icon_file : x->target_file);
    if (icon) {
        char val[160];
        snprintf(val, sizeof val, x->icon_file.ptr ? "[#%s]" : "[#%s],0", icon);
        snprintf(rid, sizeof rid, "Icon%s", suffix);
        add_class_value(c, rp_cs(x->id), rid, base, "Icon", val, with, x->pos);
    }
    if (x->extended) {
        snprintf(rid, sizeof rid, "Shift%s", suffix);
        add_class_value(c, rp_cs(x->id), rid, base, "Extended", "", with, x->pos);
    }
    if (x->target_file.ptr == NULL) {
        snprintf(rid, sizeof rid, "Sub%s", suffix);
        add_class_value(c, rp_cs(x->id), rid, base, "SubCommands", "", with, x->pos);
        return;
    }
    if (x->multi == RP_MENU_SINGLE) {
        snprintf(rid, sizeof rid, "Multi%s", suffix);
        add_class_value(c, rp_cs(x->id), rid, base, "MultiSelectModel", "Single", with, x->pos);
    }
    // The command; on the folder background there is no "%1": the folder is "%V".
    size_t al = x->args.size;
    char *cmd = rp_mem_alloc(c->alloc, x->target_file.size + al + 16, 1);
    if (cmd == NULL) {
        c->nomem = true;
        return;
    }
    size_t w = (size_t)sprintf(cmd, "\"[#%s]\" ", rp_cs(x->target_file));
    for (const char *p = rp_cs(x->args); *p; ++p) {
        cmd[w++] = *p;
        if (background && p[0] == '%' && p[1] == '1') {
            cmd[w++] = 'V';
            ++p;
        }
    }
    cmd[w] = '\0';
    snprintf(key, sizeof key, "%s\\command", base);
    snprintf(rid, sizeof rid, "Cmd%s", suffix);
    add_class_value(c, rp_cs(x->id), rid, key, NULL, cmd, with, x->pos);
    rp_mem_free(c->alloc, cmd);
}

// An MSIX declares each top-level item as a context menu verb of rubrapack's menu DLL: the same
// [msix-extension] an author's own DLL would get (kind = "context-menu").
static void menu_msix_exts(ctx_t *c) {
    rp_ir_t *ir = c->ir;
    if (file_by_id(ir, "RpMenuDll") == NULL) return;
    for (size_t k = 0; k < ir->menu_count && !c->nomem; ++k) {
        const rp_ir_menu_t *x = &ir->menus[k];
        if (x->parent.ptr || !x->windows11 || x->clsid.ptr == NULL) continue;
        rp_ir_msix_ext_t *e = &ir->msix_exts[ir->msix_ext_count++];
        memset(e, 0, sizeof *e);
        char id[64];
        snprintf(id, sizeof id, "RpMenu_%s", rp_cs(x->id));
        e->id = RP_V(ir_dup(c, id));
        e->kind = RP_MSIX_EXT_CONTEXT_MENU;
        e->file = RP_V(ir_dup(c, "RpMenuDll"));
        e->clsid = RP_V(ir_dup(c, rp_cs(x->clsid)));
        e->threading = RP_V(ir_dup(c, "STA"));
        e->verb = RP_V(ir_dup(c, rp_cs(x->id)));
        e->pos = x->pos;
        e->types = rp_mem_alloc(c->alloc, x->on_count ? x->on_count : 1, sizeof *e->types);
        if (e->types == NULL) {
            c->nomem = true;
            return;
        }
        for (size_t p = 0; p < x->on_count; ++p) {
            const char *on = x->on[p], *type = on;
            if (strcmp(on, "drive") == 0) continue;                 // no item type for it there
            if (strcmp(on, "folder") == 0) type = "Directory";
            else if (strcmp(on, "background") == 0) type = "Directory\\Background";
            else if (strncmp(on, "assoc:", 6) == 0) {
                for (size_t a = 0; a < ir->assoc_count; ++a) {
                    if (RP_EQ(ir->assocs[a].id, on + 6)) type = rp_cs(ir->assocs[a].extension);
                }
            }
            bool seen = false;
            for (size_t u = 0; u < e->type_count; ++u) seen |= strcmp(e->types[u], type) == 0;
            if (!seen) e->types[e->type_count++] = ir_dup(c, type);
        }
    }
}

static void menu_values(ctx_t *c) {
    rp_ir_t *ir = c->ir;
    char token[48];
    rp_menu_token(ir, token);
    menu_msix_exts(c);
    for (size_t k = 0; k < ir->menu_count && !c->nomem; ++k) {
        const rp_ir_menu_t *x = &ir->menus[k];
        if (x->parent.ptr) continue;
        // The component the values go with: the item's program, or a sub-menu's first item's.
        const char *with = rp_cs(x->target_file);
        for (size_t j = 0; with == NULL && j < ir->menu_count; ++j) {
            if (ir->menus[j].parent.ptr && RP_EQ(ir->menus[j].parent, x->id)) with = rp_cs(ir->menus[j].target_file);
        }
        if (ir->menu_package && x->windows11 && x->clsid.ptr) {
            char key[128], sub[200];
            snprintf(key, sizeof key, "Software\\rubrapack\\Menu\\%s", rp_cs(x->clsid));
            menu_data_at(c, x, key, "D", with);
            for (size_t j = 0; j < ir->menu_count && !c->nomem; ++j) {
                const rp_ir_menu_t *y = &ir->menus[j];
                if (y->parent.ptr == NULL || !RP_EQ(y->parent, x->id)) continue;
                snprintf(sub, sizeof sub, "%s\\%s", key, rp_cs(y->id));
                menu_data_at(c, y, sub, "D", rp_cs(y->target_file));
            }
        }
        for (size_t p = 0; p < x->on_count && !c->nomem; ++p) {
            const char *on = x->on[p], *place = on;
            char sys[96];
            if (strcmp(on, "folder") == 0) place = "Directory";
            else if (strcmp(on, "background") == 0) place = "Directory\\Background";
            else if (strcmp(on, "drive") == 0) place = "Drive";
            else if (strncmp(on, "assoc:", 6) == 0) {
                for (size_t a = 0; a < ir->assoc_count; ++a) {
                    if (RP_EQ(ir->assocs[a].id, on + 6)) place = rp_cs(ir->assocs[a].prog_id);
                }
            } else if (on[0] == '.') {
                snprintf(sys, sizeof sys, "SystemFileAssociations\\%s", on);
                place = sys;
            }
            bool background = strcmp(on, "background") == 0;
            char base[320], suffix[24];
            snprintf(base, sizeof base, "%s\\shell\\%s.%s", place, token, rp_cs(x->id));
            snprintf(suffix, sizeof suffix, "%zu", p);
            menu_values_at(c, x, base, suffix, background, with);
            for (size_t j = 0; j < ir->menu_count && !c->nomem; ++j) {
                const rp_ir_menu_t *y = &ir->menus[j];
                if (y->parent.ptr == NULL || !RP_EQ(y->parent, x->id)) continue;
                char sub[400];
                snprintf(sub, sizeof sub, "%s\\shell\\%s", base, rp_cs(y->id));
                menu_values_at(c, y, sub, suffix, background, rp_cs(y->target_file));
            }
        }
    }
}

void ir_class_checks(ctx_t *c) {
    rp_ir_t *ir = c->ir;
    menu_checks(c);
    ir_more_checks(c);
    // [other-copy.*]: the preflight looks; this product's own upgrade code is its own upgrade.
    for (size_t k = 0; k < ir->other_count; ++k) {
        const rp_ir_other_t *x = &ir->others[k];
        if (ir->no_preflight) ERR(c, x->pos, "RP1316", "[other-copy.%s]: the preflight looks for the other copy: it needs preflight = true", rp_cs(x->id));
        if (x->upgrade_code.ptr && ir->upgrade_code.ptr && RP_EQ(x->upgrade_code, ir->upgrade_code))
            ERR(c, x->pos, "RP1316", "[other-copy.%s]: upgrade-code is this package's own (that is an upgrade, not another copy)", rp_cs(x->id));
    }
    if (ir->ui_launch_file.ptr) program_ok(c, rp_cs(ir->ui_launch_file), (rp_pos_t){ 1, 1 }, "[ui] launch");
    for (size_t k = 0; k < ir->assoc_count; ++k) {
        rp_ir_assoc_t *x = &ir->assocs[k];
        program_ok(c, rp_cs(x->target_file), x->pos, "target");
        if (x->icon_file.ptr && file_by_id(ir, rp_cs(x->icon_file)) == NULL) ERR(c, x->pos, "RP1315", "icon: file '%s' is not a [file.*] of this package", rp_cs(x->icon_file));
        for (size_t j = 0; j < k; ++j) {
            const rp_ir_assoc_t *y = &ir->assocs[j];
            if (x->extension.ptr && y->extension.ptr && RP_EQ(x->extension, y->extension)) {
                ERR(c, x->pos, "RP1301", "extension '%s' already has a program ([assoc.%s])", rp_cs(x->extension), rp_cs(y->id));
            }
            if (x->prog_id.ptr && y->prog_id.ptr && RP_EQ(x->prog_id, y->prog_id) &&
                (ir_cmp_str(rp_cs(x->description), rp_cs(y->description)) != 0 || ir_cmp_str(rp_cs(x->target_file), rp_cs(y->target_file)) != 0 ||
                 ir_cmp_str(rp_cs(x->icon_file), rp_cs(y->icon_file)) != 0 || ir_cmp_str(rp_cs(x->args), rp_cs(y->args)) != 0)) {
                ERR(c, x->pos, "RP1316", "prog-id '%s' is also in [assoc.%s] with another description, target, icon or args", rp_cs(x->prog_id), rp_cs(y->id));
            } else if (x->prog_id.ptr && y->prog_id.ptr && RP_EQ(x->prog_id, y->prog_id) && ir_cmp_str(rp_cs(x->logo), rp_cs(y->logo)) != 0) {
                ERR(c, x->pos, "RP1316", "prog-id '%s' is also in [assoc.%s] with another logo", rp_cs(x->prog_id), rp_cs(y->id));
            }
        }
    }
    for (size_t k = 0; k < ir->protocol_count; ++k) {
        rp_ir_protocol_t *x = &ir->protocols[k];
        program_ok(c, rp_cs(x->target_file), x->pos, "target");
        for (size_t j = 0; j < k; ++j) {
            if (x->name.ptr && ir->protocols[j].name.ptr && RP_EQ(x->name, ir->protocols[j].name)) {
                ERR(c, x->pos, "RP1301", "scheme '%s' already has a program ([protocol.%s])", rp_cs(x->name), rp_cs(ir->protocols[j].id));
            }
        }
    }
    for (size_t k = 0; k < ir->msix_ext_count; ++k) {
        rp_ir_msix_ext_t *x = &ir->msix_exts[k];
        bool found = x->app.ptr == NULL;
        for (size_t j = 0; !found && j < ir->msix_app_count; ++j) found = RP_EQ(ir->msix_apps[j].id, x->app);
        if (!found) ERR(c, x->pos, "RP1315", "app '%s' is not an [msix-app.*]", rp_cs(x->app));
        for (size_t j = 0; j < k; ++j) {
            const rp_ir_msix_ext_t *y = &ir->msix_exts[j];
            if (x->kind == RP_MSIX_EXT_ALIAS && y->kind == RP_MSIX_EXT_ALIAS && ir_cmp_str(rp_cs(x->app), rp_cs(y->app)) == 0) {
                ERR(c, x->pos, "RP1301", "an application has one alias ([msix-extension.%s] is one already)", rp_cs(y->id));
            }
            if (x->kind == RP_MSIX_EXT_SHARE_TARGET && y->kind == RP_MSIX_EXT_SHARE_TARGET && ir_cmp_str(rp_cs(x->app), rp_cs(y->app)) == 0) {
                ERR(c, x->pos, "RP1301", "an application has one share target ([msix-extension.%s] is one already)", rp_cs(y->id));
            }
            if (x->kind == RP_MSIX_EXT_STARTUP && y->kind == RP_MSIX_EXT_STARTUP && RP_EQ(x->task_id, y->task_id)) {
                ERR(c, x->pos, "RP1301", "task-id '%s' is already used by [msix-extension.%s]", rp_cs(x->task_id), rp_cs(y->id));
            }
        }
    }
    // [com.*] (RFC-0022): a served class of this package, each class and prog-id once.
    for (size_t k = 0; k < ir->com_count; ++k) {
        rp_ir_com_t *x = &ir->coms[k];
        const rp_ir_file_t *f = file_by_id(ir, rp_cs(x->file));
        const char *n = f && f->name.ptr ? rp_cs(f->name) : "";
        size_t l = strlen(n);
        bool exe = l >= 4 && (strcmp(n + l - 4, ".exe") == 0 || strcmp(n + l - 4, ".EXE") == 0);
        bool dll = l >= 4 && (strcmp(n + l - 4, ".dll") == 0 || strcmp(n + l - 4, ".DLL") == 0);
        if (x->file.ptr && f == NULL) ERR(c, x->pos, "RP1315", "file: '%s' is not a [file.*] of this package", rp_cs(x->file));
        else if (f && !exe && !dll) ERR(c, x->pos, "RP1316", "file: '%s' serves COM classes only as an .exe or a .dll", rp_cs(x->file));
        x->exe = exe;
        if (exe && x->threading.ptr) ERR(c, x->pos, "RP1316", "[com.%s]: threading is for a DLL; a program sets its own apartment", rp_cs(x->id));
        if (exe && x->surrogate) ERR(c, x->pos, "RP1316", "[com.%s]: surrogate runs a DLL in dllhost; '%s' is a program", rp_cs(x->id), rp_cs(x->file));
        if (dll && x->args.ptr) ERR(c, x->pos, "RP1316", "[com.%s]: args are a program's; a DLL is loaded, not started", rp_cs(x->id));
        if (x->typelib_file.ptr && file_by_id(ir, rp_cs(x->typelib_file)) == NULL) {
            ERR(c, x->pos, "RP1315", "typelib-file: '%s' is not a [file.*] of this package", rp_cs(x->typelib_file));
        }
        for (size_t j = 0; j < k; ++j) {
            const rp_ir_com_t *y = &ir->coms[j];
            if (x->clsid.ptr && y->clsid.ptr && RP_EQ(x->clsid, y->clsid)) ERR(c, x->pos, "RP1301", "class %s is already served by [com.%s]", rp_cs(x->clsid), rp_cs(y->id));
            if (x->prog_id.ptr && y->prog_id.ptr && RP_EQ(x->prog_id, y->prog_id)) ERR(c, x->pos, "RP1301", "prog-id '%s' is already [com.%s]'s", rp_cs(x->prog_id), rp_cs(y->id));
        }
        for (size_t j = 0; j < ir->msix_ext_count; ++j) {
            const rp_ir_msix_ext_t *y = &ir->msix_exts[j];
            if (x->clsid.ptr && y->clsid.ptr && RP_EQ(x->clsid, y->clsid)) ERR(c, x->pos, "RP1301", "class %s is also in [msix-extension.%s]", rp_cs(x->clsid), rp_cs(y->id));
        }
        for (size_t j = 0; x->prog_id.ptr && j < ir->assoc_count; ++j) {
            if (ir->assocs[j].prog_id.ptr && RP_EQ(ir->assocs[j].prog_id, x->prog_id)) {
                ERR(c, x->pos, "RP1301", "prog-id '%s' is also [assoc.%s]'s: a file type and a COM class need two names", rp_cs(x->prog_id), rp_cs(ir->assocs[j].id));
            }
        }
    }
    // [handler.*] (RFC-0024): a DLL class of a [com.*], one handler of a kind per file type.
    for (size_t k = 0; k < ir->handler_count; ++k) {
        const rp_ir_handler_t *x = &ir->handlers[k];
        const rp_ir_com_t *cm = com_by_class(ir, rp_cs(x->clsid));
        static const char *const kinds[] = { "preview", "thumbnail", "property" };
        if (x->clsid.ptr && cm == NULL) {
            ERR(c, x->pos, "RP1315", "[handler.%s]: class %s is not a [com.*] of this package (the handler's DLL serves it)", rp_cs(x->id), rp_cs(x->clsid));
        } else if (cm && cm->exe) {
            ERR(c, x->pos, "RP1316", "[handler.%s]: Explorer loads a %s handler from a DLL; [com.%s] is a program", rp_cs(x->id), kinds[x->kind], rp_cs(cm->id));
        } else if (cm && x->kind == RP_HANDLER_PREVIEW && (cm->app_id.ptr || cm->surrogate)) {
            ERR(c, x->pos, "RP1316", "[handler.%s]: a preview handler runs in Windows' prevhost; leave app-id and surrogate out of [com.%s]", rp_cs(x->id), rp_cs(cm->id));
        }
        if (x->kind == RP_HANDLER_PROPERTY && ir->scope != 0) {
            ERR(c, x->pos, "RP1316", "[handler.%s]: Windows reads property handlers per machine only; the package needs scope = \"machine\"", rp_cs(x->id));
        }
        for (size_t j = 0; j < k; ++j) {
            const rp_ir_handler_t *y = &ir->handlers[j];
            if (y->kind != x->kind) continue;
            for (size_t a = 0; a < x->type_count; ++a) {
                for (size_t b = 0; b < y->type_count; ++b) {
                    if (x->types[a] && y->types[b] && strcmp(x->types[a], y->types[b]) == 0) {
                        ERR(c, x->pos, "RP1301", "'%s' already has a %s handler ([handler.%s])", x->types[a], kinds[x->kind], rp_cs(y->id));
                    }
                }
            }
        }
    }
    if (c->d->errors) return;       // a table with a missing key has NULL fields; the build fails anyway
    menu_values(c);
    for (size_t k = 0; k < ir->assoc_count && !c->nomem; ++k) {
        const rp_ir_assoc_t *x = &ir->assocs[k];
        // The extension names the ProgID as its own (unless default = false) and always lists it
        // among those that open it ("Open with"); Windows 10 and 11 let only the user pick the default.
        char okey[96];
        snprintf(okey, sizeof okey, "%s\\OpenWithProgids", rp_cs(x->extension));
        if (!x->no_default) add_class_value(c, rp_cs(x->id), "Ext", rp_cs(x->extension), NULL, rp_cs(x->prog_id), rp_cs(x->target_file), x->pos);
        add_class_value(c, rp_cs(x->id), "With", okey, rp_cs(x->prog_id), "", rp_cs(x->target_file), x->pos);
        if (x->content_type.ptr) add_class_value(c, rp_cs(x->id), "Type", rp_cs(x->extension), "Content Type", rp_cs(x->content_type), rp_cs(x->target_file), x->pos);
        if (x->perceived_type.ptr) add_class_value(c, rp_cs(x->id), "Kind", rp_cs(x->extension), "PerceivedType", rp_cs(x->perceived_type), rp_cs(x->target_file), x->pos);
        bool first = true;              // a prog-id shared by several extensions is written once
        for (size_t j = 0; j < k; ++j) first &= !RP_EQ(ir->assocs[j].prog_id, x->prog_id);
        if (!first) continue;
        char key[160], val[512];
        add_class_value(c, rp_cs(x->id), "Prog", rp_cs(x->prog_id), NULL, rp_cs(x->description.ptr ? x->description : ir->name), rp_cs(x->target_file), x->pos);
        snprintf(key, sizeof key, "%s\\DefaultIcon", rp_cs(x->prog_id));
        snprintf(val, sizeof val, "[#%s],0", rp_cs(x->icon_file.ptr ? x->icon_file : x->target_file));
        add_class_value(c, rp_cs(x->id), "Icon", key, NULL, val, rp_cs(x->target_file), x->pos);
        snprintf(key, sizeof key, "%s\\shell\\open\\command", rp_cs(x->prog_id));
        char *cmd = rp_mem_alloc(c->alloc, x->target_file.size + x->args.size + 16, 1);
        if (cmd == NULL) {
            c->nomem = true;
            return;
        }
        sprintf(cmd, "\"[#%s]\" %s", rp_cs(x->target_file), rp_cs(x->args));
        add_class_value(c, rp_cs(x->id), "Cmd", key, NULL, cmd, rp_cs(x->target_file), x->pos);
        rp_mem_free(c->alloc, cmd);
    }
    for (size_t k = 0; k < ir->protocol_count && !c->nomem; ++k) {
        const rp_ir_protocol_t *x = &ir->protocols[k];
        char key[160], val[512];
        snprintf(val, sizeof val, "URL:%s", rp_cs(x->description.ptr ? x->description : x->name));
        add_class_value(c, rp_cs(x->id), "Url", rp_cs(x->name), NULL, val, rp_cs(x->target_file), x->pos);
        add_class_value(c, rp_cs(x->id), "Proto", rp_cs(x->name), "URL Protocol", "", rp_cs(x->target_file), x->pos);
        snprintf(key, sizeof key, "%s\\DefaultIcon", rp_cs(x->name));
        snprintf(val, sizeof val, "[#%s],0", rp_cs(x->target_file));
        add_class_value(c, rp_cs(x->id), "Icon", key, NULL, val, rp_cs(x->target_file), x->pos);
        snprintf(key, sizeof key, "%s\\shell\\open\\command", rp_cs(x->name));
        char *cmd = rp_mem_alloc(c->alloc, x->target_file.size + x->args.size + 16, 1);
        if (cmd == NULL) {
            c->nomem = true;
            return;
        }
        sprintf(cmd, "\"[#%s]\" %s", rp_cs(x->target_file), rp_cs(x->args));
        add_class_value(c, rp_cs(x->id), "Cmd", key, NULL, cmd, rp_cs(x->target_file), x->pos);
        rp_mem_free(c->alloc, cmd);
    }
    // [handler.*]: Explorer finds a handler under the file type's ShellEx key (preview, thumbnail),
    // or in PropertySystem\PropertyHandlers (property, HKLM); a preview handler is also on the
    // PreviewHandlers list and runs in prevhost (its class's AppID). All in the DLL's component.
    for (size_t k = 0; k < ir->handler_count && !c->nomem; ++k) {
        const rp_ir_handler_t *x = &ir->handlers[k];
        const rp_ir_com_t *cm = com_by_class(ir, rp_cs(x->clsid));
        const char *desc = rp_cs(x->description.ptr ? x->description : cm->description.ptr ? cm->description : ir->name);
        char key[256], suffix[24];
        for (size_t t = 0; t < x->type_count; ++t) {
            snprintf(suffix, sizeof suffix, "T%zu", t + 1);
            if (x->kind == RP_HANDLER_PROPERTY) {
                snprintf(key, sizeof key, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\PropertySystem\\PropertyHandlers\\%s", x->types[t]);
                add_root_value(c, rp_cs(x->id), suffix, RP_ROOT_HKLM, key, NULL, rp_cs(x->clsid), rp_cs(cm->file), x->pos);
            } else {
                snprintf(key, sizeof key, "%s\\ShellEx\\%s", x->types[t],
                         x->kind == RP_HANDLER_PREVIEW ? "{8895B1C6-B41F-4C1C-A562-0D564250836F}" : "{E357FCCD-A995-4576-B01F-234630154E96}");
                add_class_value(c, rp_cs(x->id), suffix, key, NULL, rp_cs(x->clsid), rp_cs(cm->file), x->pos);
            }
        }
        if (x->kind == RP_HANDLER_PREVIEW) {
            snprintf(key, sizeof key, "CLSID\\%s", rp_cs(x->clsid));
            // prevhost.exe: the 64-bit one for an x64 or Arm64 package, the 32-bit one for x86.
            add_class_value(c, rp_cs(x->id), "App", key, "AppID",
                            ir->arch == RP_ARCH_X86 ? "{534A1E02-D58F-44F0-B58B-36CBED287C7C}" : "{6D2B5079-2F0B-48DD-AB7F-97CEC514D30B}", rp_cs(cm->file), x->pos);
            add_class_value(c, rp_cs(x->id), "Name", key, "DisplayName", desc, rp_cs(cm->file), x->pos);
            add_root_value(c, rp_cs(x->id), "List", RP_ROOT_HKMU, "Software\\Microsoft\\Windows\\CurrentVersion\\PreviewHandlers", rp_cs(x->clsid), desc,
                           rp_cs(cm->file), x->pos);
        }
    }
    // [com.*]: HKCR values for the MSI in the server's component, and a com-server extension for the MSIX.
    for (size_t k = 0; k < ir->com_count && !c->nomem; ++k) {
        const rp_ir_com_t *x = &ir->coms[k];
        const char *desc = rp_cs(x->description.ptr ? x->description : ir->name);
        char key[200], val[512];
        snprintf(key, sizeof key, "CLSID\\%s", rp_cs(x->clsid));
        add_class_value(c, rp_cs(x->id), "Cls", key, NULL, desc, rp_cs(x->file), x->pos);
        if (x->exe) {
            snprintf(key, sizeof key, "CLSID\\%s\\LocalServer32", rp_cs(x->clsid));
            char *cmd = rp_mem_alloc(c->alloc, x->file.size + (x->args.ptr ? x->args.size : 0) + 16, 1);
            if (cmd == NULL) {
                c->nomem = true;
                return;
            }
            sprintf(cmd, x->args.ptr ? "\"[#%s]\" %s" : "\"[#%s]\"", rp_cs(x->file), rp_cs(x->args));
            add_class_value(c, rp_cs(x->id), "Srv", key, NULL, cmd, rp_cs(x->file), x->pos);
            rp_mem_free(c->alloc, cmd);
        } else {
            snprintf(key, sizeof key, "CLSID\\%s\\InprocServer32", rp_cs(x->clsid));
            snprintf(val, sizeof val, "[#%s]", rp_cs(x->file));
            add_class_value(c, rp_cs(x->id), "Srv", key, NULL, val, rp_cs(x->file), x->pos);
            add_class_value(c, rp_cs(x->id), "Thr", key, "ThreadingModel", x->threading.ptr ? rp_cs(x->threading) : "Apartment", rp_cs(x->file), x->pos);
        }
        if (x->prog_id.ptr) {
            snprintf(key, sizeof key, "CLSID\\%s\\ProgID", rp_cs(x->clsid));
            add_class_value(c, rp_cs(x->id), "PidC", key, NULL, rp_cs(x->prog_id), rp_cs(x->file), x->pos);
            add_class_value(c, rp_cs(x->id), "Pid", rp_cs(x->prog_id), NULL, desc, rp_cs(x->file), x->pos);
            snprintf(key, sizeof key, "%s\\CLSID", rp_cs(x->prog_id));
            add_class_value(c, rp_cs(x->id), "PidK", key, NULL, rp_cs(x->clsid), rp_cs(x->file), x->pos);
        }
        const char *app = x->app_id.ptr ? rp_cs(x->app_id) : x->surrogate ? rp_cs(x->clsid) : NULL;
        if (app) {
            snprintf(key, sizeof key, "CLSID\\%s", rp_cs(x->clsid));
            add_class_value(c, rp_cs(x->id), "App", key, "AppID", app, rp_cs(x->file), x->pos);
            snprintf(key, sizeof key, "AppID\\%s", app);
            add_class_value(c, rp_cs(x->id), "AppK", key, NULL, desc, rp_cs(x->file), x->pos);
            if (x->surrogate) add_class_value(c, rp_cs(x->id), "Sur", key, "DllSurrogate", "", rp_cs(x->file), x->pos);
        }
        if (x->typelib.ptr) {
            const char *tf = rp_cs(x->typelib_file.ptr ? x->typelib_file : x->file);
            snprintf(key, sizeof key, "CLSID\\%s\\TypeLib", rp_cs(x->clsid));
            add_class_value(c, rp_cs(x->id), "Tlb", key, NULL, rp_cs(x->typelib), rp_cs(x->file), x->pos);
            snprintf(key, sizeof key, "TypeLib\\%s\\%s", rp_cs(x->typelib), rp_cs(x->typelib_version));
            add_class_value(c, rp_cs(x->id), "TlbV", key, NULL, desc, tf, x->pos);
            snprintf(key, sizeof key, "TypeLib\\%s\\%s\\0\\%s", rp_cs(x->typelib), rp_cs(x->typelib_version), ir->arch == RP_ARCH_X86 ? "win32" : "win64");
            snprintf(val, sizeof val, "[#%s]", tf);
            add_class_value(c, rp_cs(x->id), "TlbP", key, NULL, val, tf, x->pos);
            snprintf(key, sizeof key, "TypeLib\\%s\\%s\\FLAGS", rp_cs(x->typelib), rp_cs(x->typelib_version));
            add_class_value(c, rp_cs(x->id), "TlbF", key, NULL, "0", tf, x->pos);
            snprintf(key, sizeof key, "TypeLib\\%s\\%s\\HELPDIR", rp_cs(x->typelib), rp_cs(x->typelib_version));
            const rp_ir_file_t *tff = file_by_id(ir, tf);
            snprintf(val, sizeof val, "[%s]", tff ? rp_cs(tff->dir) : "INSTALLDIR");
            add_class_value(c, rp_cs(x->id), "TlbH", key, NULL, val, tf, x->pos);
        }
        if (x->msi_only) continue;
        rp_ir_msix_ext_t *e = &ir->msix_exts[ir->msix_ext_count++];
        memset(e, 0, sizeof *e);
        e->id = RP_V(ir_dup(c, rp_cs(x->id)));
        e->kind = RP_MSIX_EXT_COM;
        e->file = RP_V(ir_dup(c, rp_cs(x->file)));
        e->clsid = RP_V(ir_dup(c, rp_cs(x->clsid)));
        e->display = RP_V(ir_dup(c, desc));
        static const char *const msix_models[][2] = { { "Apartment", "STA" }, { "Free", "MTA" }, { "Both", "Both" }, { "Neutral", "Neutral" } };
        for (size_t m = 0; m < 4; ++m) {
            if (strcmp(x->threading.ptr ? rp_cs(x->threading) : "Apartment", msix_models[m][0]) == 0) e->threading = RP_V(ir_dup(c, msix_models[m][1]));
        }
        if (x->args.ptr) e->args = RP_V(ir_dup(c, rp_cs(x->args)));
        if (x->prog_id.ptr) e->prog_id = RP_V(ir_dup(c, rp_cs(x->prog_id)));
        e->pos = x->pos;
    }
}

typedef struct { const char *id; rp_pos_t pos; } idpos_t;
typedef struct { const char *id; size_t at; } idat_t;     // an ID and its place in the list
typedef struct { size_t a, b; } dup_t;                      // two places of one ID, a before b

static int cmp_idat(const void *x, const void *y) {
    const idat_t *a = x, *b = y;
    int r = strcmp(a->id, b->id);
    return r ? r : a->at < b->at ? -1 : a->at > b->at;
}

static int cmp_dup(const void *x, const void *y) {
    const dup_t *a = x, *b = y;
    if (a->a != b->a) return a->a < b->a ? -1 : 1;
    return a->b < b->b ? -1 : a->b > b->b;
}

// Every pair a < b with v[a] equal to v[b] (NULLs left out), ordered by a then b; NULL with
// nothing in *out when there is none. Sorting first keeps this n log n for many distinct strings.
static size_t equal_pairs(ctx_t *c, const char *const *v, size_t n, dup_t **out) {
    *out = NULL;
    idat_t *by = rp_mem_alloc(c->alloc, n ? n : 1, sizeof *by);
    if (by == NULL) {
        c->nomem = true;
        return 0;
    }
    size_t m = 0;
    for (size_t k = 0; k < n; ++k) {
        if (v[k]) by[m++] = (idat_t){ v[k], k };
    }
    rp_sort(by, m, sizeof *by, cmp_idat);
    dup_t *dups = NULL;
    size_t nd = 0, cap = 0;
    for (size_t g = 0; g < m && !c->nomem;) {
        size_t e = g + 1;
        while (e < m && strcmp(by[g].id, by[e].id) == 0) ++e;
        for (size_t a = g; a < e && !c->nomem; ++a) {
            for (size_t b = a + 1; b < e; ++b) {
                if (nd == cap) {
                    size_t nc = cap ? cap * 2 : 8;
                    dup_t *w = rp_mem_alloc(c->alloc, nc, sizeof *w);
                    if (w == NULL) {
                        c->nomem = true;
                        break;
                    }
                    if (nd) memcpy(w, dups, nd * sizeof *w);
                    rp_mem_free(c->alloc, dups);
                    dups = w;
                    cap = nc;
                }
                dups[nd++] = (dup_t){ by[a].at, by[b].at };
            }
        }
        g = e;
    }
    rp_mem_free(c->alloc, by);
    rp_sort(dups, nd, sizeof *dups, cmp_dup);
    *out = dups;
    return nd;
}

void ir_cross_checks(ctx_t *c) {
    rp_ir_t *ir = c->ir;
    // [arp] no-remove leaves Modify as the way to remove the product: it needs dialogs and Modify.
    if (ir->arp_no_remove && ir->arp_no_modify)
        ERR(c, ir->arp_no_remove_pos, "RP1316", "no-remove leaves Modify (its Remove) as the only way to remove the product: it cannot go with no-modify");
    // IDs unique across dir, file and feature (RFC-0002 2).
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
    for (size_t k = 0; k < ir->dir_count; ++k) ids[m++] = (idpos_t){ rp_cs(ir->dirs[k].id), ir->dirs[k].pos };
    for (size_t k = 0; k < ir->file_count; ++k) ids[m++] = (idpos_t){ rp_cs(ir->files[k].id), ir->files[k].pos };
    for (size_t k = 0; k < ir->folder_count; ++k) ids[m++] = (idpos_t){ rp_cs(ir->folders[k].id), ir->folders[k].pos };
    for (size_t k = 0; k < ir->registry_count; ++k) ids[m++] = (idpos_t){ rp_cs(ir->registries[k].id), ir->registries[k].pos };
    for (size_t k = 0; k < ir->shortcut_count; ++k) ids[m++] = (idpos_t){ rp_cs(ir->shortcuts[k].id), ir->shortcuts[k].pos };
    for (size_t k = 0; k < ir->remove_count; ++k) ids[m++] = (idpos_t){ rp_cs(ir->removes[k].id), ir->removes[k].pos };
    for (size_t k = 0; k < ir->copy_count; ++k) ids[m++] = (idpos_t){ rp_cs(ir->copies[k].id), ir->copies[k].pos };
    for (size_t k = 0; k < ir->env_count; ++k) ids[m++] = (idpos_t){ rp_cs(ir->envs[k].id), ir->envs[k].pos };
    for (size_t k = 0; k < ir->ini_count; ++k) ids[m++] = (idpos_t){ rp_cs(ir->inis[k].id), ir->inis[k].pos };
    for (size_t k = 0; k < ir->require_count; ++k) ids[m++] = (idpos_t){ rp_cs(ir->requires[k].id), ir->requires[k].pos };
    for (size_t k = 0; k < ir->search_count; ++k) ids[m++] = (idpos_t){ rp_cs(ir->searches[k].id), ir->searches[k].pos };
    for (size_t k = 0; k < ir->service_count; ++k) ids[m++] = (idpos_t){ rp_cs(ir->services[k].id), ir->services[k].pos };
    for (size_t k = 0; k < ir->font_count; ++k) ids[m++] = (idpos_t){ rp_cs(ir->fonts[k].id), ir->fonts[k].pos };
    for (size_t k = 0; k < ir->assoc_count; ++k) ids[m++] = (idpos_t){ rp_cs(ir->assocs[k].id), ir->assocs[k].pos };
    for (size_t k = 0; k < ir->protocol_count; ++k) ids[m++] = (idpos_t){ rp_cs(ir->protocols[k].id), ir->protocols[k].pos };
    for (size_t k = 0; k < ir->msix_ext_count; ++k) ids[m++] = (idpos_t){ rp_cs(ir->msix_exts[k].id), ir->msix_exts[k].pos };
    for (size_t k = 0; k < ir->permission_count; ++k) ids[m++] = (idpos_t){ rp_cs(ir->permissions[k].id), ir->permissions[k].pos };
    for (size_t k = 0; k < ir->dialog_count; ++k) ids[m++] = (idpos_t){ rp_cs(ir->dialogs[k].id), ir->dialogs[k].pos };
    for (size_t k = 0; k < ir->dialog_control_count; ++k) ids[m++] = (idpos_t){ rp_cs(ir->dialog_controls[k].id), ir->dialog_controls[k].pos };
    for (size_t k = 0; k < ir->feature_count; ++k) {
        if (!ir->features[k].implicit) ids[m++] = (idpos_t){ rp_cs(ir->features[k].id), ir->features[k].pos };
    }
    const char **idv = rp_mem_alloc(c->alloc, m ? m : 1, sizeof *idv);
    dup_t *dups = NULL;
    size_t ndup = 0;
    if (idv == NULL) c->nomem = true;
    else {
        for (size_t k = 0; k < m; ++k) idv[k] = ids[k].id;
        ndup = equal_pairs(c, idv, m, &dups);
    }
    for (size_t k = 0; k < ndup; ++k) {
        ERR(c, ids[dups[k].b].pos, "RP1301", "ID '%s' is already used (line %u); IDs must differ across all tables",
            ids[dups[k].b].id, (unsigned)ids[dups[k].a].pos.line);
    }
    rp_mem_free(c->alloc, dups);
    rp_mem_free(c->alloc, idv);
    rp_mem_free(c->alloc, ids);

    // Features: parents exist, no loops.
    for (size_t k = 0; k < ir->feature_count; ++k) {
        const rp_ir_feature_t *f = &ir->features[k];
        size_t steps = 0;
        for (const rp_ir_feature_t *p = f; p && p->parent.ptr; p = find_feature(ir, rp_cs(p->parent))) {
            if (!find_feature(ir, rp_cs(p->parent))) {
                ERR(c, p->pos, "RP1307", "feature parent '%s' is not defined", rp_cs(p->parent));
                break;
            }
            if (++steps > ir->feature_count) {
                ERR(c, f->pos, "RP1303", "feature '%s' is its own ancestor", rp_cs(f->id));
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
        if (d->parent.ptr && !rp_ir_find_dir(ir, rp_cs(d->parent))) {
            ERR(c, d->pos, "RP1307", "'%s' is neither a known folder nor a dir ID (known folders: ProgramFiles, "
                "ProgramFiles32, CommonFiles, AppData, LocalAppData, CommonAppData, StartMenu, Programs, Desktop, "
                "Windows, System, Fonts, Temp)", rp_cs(d->parent));
            continue;
        }
        dir_paths[k] = dir_full_path(c, d, 0);
        if (dir_paths[k] == NULL && d->parent.ptr) ERR(c, d->pos, "RP1303", "dir '%s' is inside itself (path loop)", rp_cs(d->id));
        if (d->feature.ptr && !find_feature(ir, rp_cs(d->feature))) {
            ERR(c, d->pos, "RP1307", "feature '%s' is not defined", rp_cs(d->feature));
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
        if (folded[nf]) ir_fold(dir_paths[k], folded[nf], strlen(dir_paths[k]) * 2 + 4);
        where[nf++] = ir->dirs[k].pos;
    }
    for (size_t k = 0; k < ir->file_count; ++k) {
        rp_ir_file_t *f = &ir->files[k];
        const rp_ir_dir_t *d = f->dir.ptr ? rp_ir_find_dir(ir, rp_cs(f->dir)) : NULL;
        if (f->dir.ptr && d == NULL) {
            ERR(c, f->pos, "RP1307", "dir '%s' is not defined", rp_cs(f->dir));
            continue;
        }
        if (f->feature.ptr && !find_feature(ir, rp_cs(f->feature))) {
            ERR(c, f->pos, "RP1307", "feature '%s' is not defined", rp_cs(f->feature));
        } else if (f->feature.ptr == NULL) {
            if (d && d->feature.ptr) f->feature = RP_V(ir_dup(c, rp_cs(d->feature)));
            else if (!declared) f->feature = RP_V(ir_dup(c, "Main"));
            else ERR(c, f->pos, "RP1202", "file '%s' needs a feature: set 'feature' here or on its dir", rp_cs(f->id));
        }
        if (d && f->name.ptr && folded) {
            size_t di = (size_t)(d - ir->dirs);
            if (dir_paths[di]) {
                size_t len = strlen(dir_paths[di]) + f->name.size + 2;
                char *full = rp_mem_alloc(c->alloc, len, 1);
                if (full) {
                    snprintf(full, len, "%s/%s", dir_paths[di], rp_cs(f->name));
                    folded[nf] = rp_mem_alloc(c->alloc, len * 2 + 4, 1);
                    if (folded[nf]) ir_fold(full, folded[nf], len * 2 + 4);
                    where[nf++] = f->pos;
                    rp_mem_free(c->alloc, full);
                }
            }
        }
        // PE files: the machine type must match the package architecture unless any-arch
        // (RFC-0001 9.2); a malformed PE is refused.
        if (f->source_path.ptr && f->size >= 2) {
            uint8_t *data;
            size_t len;
            if (rp_pal_read_file(c->alloc, rp_cs(f->source_path), (size_t)f->size, &data, &len) == PROVEN_OK) {
                rp_pe_info_t pi;
                proven_err_t pe_err = rp_pe_read(data, len, &pi);
                if (pe_err == PROVEN_OK && pi.is_pe) {
                    f->pe_machine = pi.machine;
                    f->pe_is_dll = pi.is_dll;
                }
                if (pe_err != PROVEN_OK) {
                    ERR(c, f->pos, "RP1513", "'%s' looks like a program file (PE) but its headers or resources are damaged",
                        rp_cs(f->source));
                } else if (pi.is_pe && !f->any_arch) {
                    uint16_t want = ir->arch == RP_ARCH_X64 ? RP_PE_AMD64 : ir->arch == RP_ARCH_ARM64 ? RP_PE_ARM64 : RP_PE_I386;
                    // A .NET assembly built for any CPU (MSIL) says i386 in its header and runs on every
                    // architecture (RFC-0031 B9).
                    rp_pe_assembly_t asm_info;
                    bool msil = rp_pe_assembly(data, len, &asm_info) == PROVEN_OK && asm_info.is_assembly && strcmp(asm_info.arch, "MSIL") == 0;
                    if (pi.machine != want && !msil) {
                        ERR(c, f->pos, "RP1512",
                            "'%s' is built for machine 0x%04X, not for this package's arch; set any-arch = true if that is intended",
                            rp_cs(f->source), pi.machine);
                    }
                }
                rp_mem_free(c->alloc, data);
            }
        }
    }
    for (size_t k = 0; k < ir->folder_count; ++k) {
        rp_ir_folder_t *f = &ir->folders[k];
        const rp_ir_dir_t *d = f->dir.ptr ? rp_ir_find_dir(ir, rp_cs(f->dir)) : NULL;
        if (f->dir.ptr && d == NULL) {
            ERR(c, f->pos, "RP1307", "dir '%s' is not defined", rp_cs(f->dir));
            continue;
        }
        if (f->feature.ptr && !find_feature(ir, rp_cs(f->feature))) {
            ERR(c, f->pos, "RP1307", "feature '%s' is not defined", rp_cs(f->feature));
        } else if (f->feature.ptr == NULL) {
            if (d && d->feature.ptr) f->feature = RP_V(ir_dup(c, rp_cs(d->feature)));
            else if (!declared) f->feature = RP_V(ir_dup(c, "Main"));
            else ERR(c, f->pos, "RP1202", "folder '%s' needs a feature: set 'feature' here or on its dir", rp_cs(f->id));
        }
        if (d && f->name.ptr && folded) {
            size_t di = (size_t)(d - ir->dirs);
            if (dir_paths[di]) {
                size_t len = strlen(dir_paths[di]) + f->name.size + 2;
                char *full = rp_mem_alloc(c->alloc, len, 1);
                if (full) {
                    snprintf(full, len, "%s/%s", dir_paths[di], rp_cs(f->name));
                    folded[nf] = rp_mem_alloc(c->alloc, len * 2 + 4, 1);
                    if (folded[nf]) ir_fold(full, folded[nf], len * 2 + 4);
                    where[nf++] = f->pos;
                    rp_mem_free(c->alloc, full);
                }
            }
        }
    }
    // Properties: not a dir/file/feature/folder ID (every directory is a property too).
    for (size_t k = 0; k < ir->property_count; ++k) {
        const rp_ir_property_t *p = &ir->properties[k];
        if (rp_ir_find_dir(ir, rp_cs(p->id)) || find_feature(ir, rp_cs(p->id))) {
            ERR(c, p->pos, "RP1310", "property '%s' has the name of a dir or feature", rp_cs(p->id));
        }
        for (size_t j = 0; j < k; ++j) {
            if (RP_EQ(ir->properties[j].id, p->id)) {
                ERR(c, p->pos, "RP1301", "property '%s' is already defined (line %u)", rp_cs(p->id), (unsigned)ir->properties[j].pos.line);
            }
        }
    }
    // Registry values: `with` names a file; otherwise the feature resolves like a file's (G2).
    for (size_t k = 0; k < ir->registry_count; ++k) {
        rp_ir_registry_t *r = &ir->registries[k];
        if (r->users_all && (!r->purge || r->root != RP_ROOT_HKCU || ir->scope != 0)) {      // RFC-0030
            ERR(c, r->pos, "RP1316", "[registry.%s]: users = \"all\" is for root = \"HKCU\" with on = \"uninstall\", in a per-machine package", rp_cs(r->id));
        }
        if (r->with_file.ptr) {
            bool found = false;
            for (size_t j = 0; j < ir->file_count; ++j) found |= RP_EQ(ir->files[j].id, r->with_file);
            if (!found) ERR(c, r->pos, "RP1315", "with: file '%s' is not a [file.*] of this package", rp_cs(r->with_file));
            if (r->view32 != (ir->arch == RP_ARCH_X86)) {
                // a component has one bitness: a 32-bit view value cannot share a 64-bit file's component
                if (r->view32) ERR(c, r->pos, "RP1316", "view = \"32\" cannot go 'with' a file of a 64-bit package");
            }
        } else if (r->feature.ptr && !find_feature(ir, rp_cs(r->feature))) {
            ERR(c, r->pos, "RP1307", "feature '%s' is not defined", rp_cs(r->feature));
        } else if (r->feature.ptr == NULL) {
            if (!declared) r->feature = RP_V(ir_dup(c, "Main"));
            else ERR(c, r->pos, "RP1202", "registry value '%s' needs a feature", rp_cs(r->id));
        }
    }
    // Removals and copies: folders and files exist; a removal's feature resolves like a file's.
    for (size_t k = 0; k < ir->remove_count; ++k) {
        rp_ir_remove_t *r = &ir->removes[k];
        const rp_ir_dir_t *d = r->dir.ptr ? rp_ir_find_dir(ir, rp_cs(r->dir)) : NULL;
        if (r->dir.ptr && d == NULL) {
            ERR(c, r->pos, "RP1315", "dir '%s' is not a dir ID", rp_cs(r->dir));
            continue;
        }
        if (r->recursive && d) {
            // Whatever it is below: Windows' own folders are never emptied.
            const rp_ir_dir_t *top = d;
            for (int hop = 0; top->parent.ptr && hop < 64; ++hop) {
                const rp_ir_dir_t *up = rp_ir_find_dir(ir, rp_cs(top->parent));
                if (up == NULL) break;
                top = up;
            }
            const char *b = rp_cs(top->base);
            if (b && (strcmp(b, "Windows") == 0 || strcmp(b, "System") == 0 || strcmp(b, "Fonts") == 0 || strcmp(b, "Temp") == 0)) {
                ERR(c, r->pos, "RP1316", "[remove.%s]: recursive = true is not for a folder below %s", rp_cs(r->id), b);
            }
        }
        // RFC-0030: every user's same folder - a folder of the profile's data, wiped as a whole, by a
        // per-machine removal (it runs as an administrator; a per-user product is each user's own).
        if (r->users_all && (!r->recursive || !d || !profile_base(ir, rp_cs(r->dir), true) || ir->scope != 0)) {
            ERR(c, r->pos, "RP1316", "[remove.%s]: users = \"all\" is for recursive = true on a folder in AppData or LocalAppData, in a per-machine package", rp_cs(r->id));
        }
        if (r->feature.ptr && !find_feature(ir, rp_cs(r->feature))) {
            ERR(c, r->pos, "RP1307", "feature '%s' is not defined", rp_cs(r->feature));
        } else if (r->feature.ptr == NULL) {
            if (d && d->feature.ptr) r->feature = RP_V(ir_dup(c, rp_cs(d->feature)));
            else if (!declared) r->feature = RP_V(ir_dup(c, "Main"));
            else ERR(c, r->pos, "RP1202", "[remove.%s] needs a feature: set 'feature' here or on its dir", rp_cs(r->id));
        }
    }
    // The user's profile (RFC-0029): a component there is keyed by an HKCU value, which a GUID the
    // source fixed does not get; a dual package installed for all users fills the installing user's
    // AppData and LocalAppData only.
    for (size_t k = 0; k < ir->file_count; ++k) {
        const rp_ir_file_t *f = &ir->files[k];
        if (f->component_guid.ptr && profile_base(ir, rp_cs(f->dir), false)) {
            rp_srcdiag_add(c->d, f->pos, "RP1320", true, "[file.%s] is in the user's profile and has component-guid: its key path stays the file, "
                           "where Windows' rules want an HKCU value (ICE38); leave component-guid out to get one", rp_cs(f->id));
        }
    }
    if (ir->scope == 2) {
        const char *what = NULL, *id = NULL;
        rp_pos_t pos = { 0 };
        for (size_t k = 0; k < ir->file_count && !what; ++k) {
            if (profile_base(ir, rp_cs(ir->files[k].dir), true)) { what = "file"; id = rp_cs(ir->files[k].id); pos = ir->files[k].pos; }
        }
        for (size_t k = 0; k < ir->folder_count && !what; ++k) {
            if (profile_base(ir, rp_cs(ir->folders[k].dir), true)) { what = "folder"; id = rp_cs(ir->folders[k].id); pos = ir->folders[k].pos; }
        }
        for (size_t k = 0; k < ir->ini_count && !what; ++k) {
            if (profile_base(ir, rp_cs(ir->inis[k].dir), true)) { what = "ini"; id = rp_cs(ir->inis[k].id); pos = ir->inis[k].pos; }
        }
        if (what) {
            rp_srcdiag_add(c->d, pos, "RP1320", true, "[%s.%s] is in the user's AppData or LocalAppData: a dual package installed for all users "
                           "puts it in the installing user's profile only", what, id);
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
            const char *b = rp_cs(ir->dirs[k].base);
            if (b && (strcmp(b, "Windows") == 0 || strcmp(b, "System") == 0 || strcmp(b, "Fonts") == 0 || strcmp(b, "CommonAppData") == 0)) {
                ERR(c, ir->dirs[k].pos, "RP1316", "'%s' is a machine folder; a %s package cannot write there", b, sc);
            }
        }
    }
    // Dialogs: the folder the user may change is a dir of this package (default: INSTALLDIR).
    if (ir->ui >= 3) {
        const char *d = ir->ui_install_dir.ptr ? rp_cs(ir->ui_install_dir) : "INSTALLDIR";
        if (!rp_ir_find_dir(ir, d)) {
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
        if (x->target.ptr == NULL) continue;
        bool found = false;
        if (x->kind == 0) {
            const rp_ir_dir_t *d = rp_ir_find_dir(ir, rp_cs(x->target));
            found = d != NULL;
            if (d && d->part_count == 0) ERR(c, x->pos, "RP1316", "a known folder itself ('%s') cannot get permissions", rp_cs(x->target));
            if (d) x->feature = RP_V(ir_dup(c, d->feature.ptr ? rp_cs(d->feature) : declared ? NULL : "Main"));
            if (d && x->feature.ptr == NULL) ERR(c, x->pos, "RP1202", "[permission.%s] needs its dir to have a feature", rp_cs(x->id));
        } else if (x->kind == 1) {
            for (size_t j = 0; j < ir->file_count; ++j) found |= RP_EQ(ir->files[j].id, x->target);
        } else {
            for (size_t j = 0; j < ir->registry_count; ++j) {
                if (RP_EQ(ir->registries[j].id, x->target)) {
                    found = true;
                    if (ir->registries[j].remove) ERR(c, x->pos, "RP1316", "registry '%s' removes a value; it has nothing to protect", rp_cs(x->target));
                }
            }
        }
        if (!found) ERR(c, x->pos, "RP1315", "target '%s' does not exist", rp_cs(x->target));
        for (size_t j = 0; j < k; ++j) {
            if (ir->permissions[j].kind == x->kind && ir->permissions[j].target.ptr && RP_EQ(ir->permissions[j].target, x->target)) {
                ERR(c, x->pos, "RP1301", "'%s' already has permissions", rp_cs(x->target));
            }
        }
    }
    // Fonts: a file installed directly in the Fonts folder ([dir.X] path = "Fonts"), once.
    for (size_t k = 0; k < ir->font_count; ++k) {
        const rp_ir_font_t *x = &ir->fonts[k];
        const rp_ir_file_t *f = NULL;
        for (size_t j = 0; x->file.ptr && j < ir->file_count; ++j) {
            if (RP_EQ(ir->files[j].id, x->file)) f = &ir->files[j];
        }
        const rp_ir_dir_t *d = f && f->dir.ptr ? rp_ir_find_dir(ir, rp_cs(f->dir)) : NULL;
        if (x->file.ptr && f == NULL) {
            ERR(c, x->pos, "RP1315", "file '%s' is not a [file.*] of this package", rp_cs(x->file));
        } else if (f && !(d && d->part_count == 0 && d->base.ptr && RP_EQ(d->base, "Fonts"))) {
            ERR(c, x->pos, "RP1316", "file '%s' must be installed in the Fonts folder itself (a [dir.*] with path = \"Fonts\")", rp_cs(x->file));
        }
        for (size_t j = 0; j < k; ++j) {
            if (x->file.ptr && ir->fonts[j].file.ptr && RP_EQ(ir->fonts[j].file, x->file)) {
                ERR(c, x->pos, "RP1301", "file '%s' is already registered as a font", rp_cs(x->file));
            }
        }
    }
    // Services: an exe of this package that its machines can run; one service per file and name.
    for (size_t k = 0; k < ir->service_count; ++k) {
        const rp_ir_service_t *x = &ir->services[k];
        const rp_ir_file_t *f = NULL;
        for (size_t j = 0; x->file.ptr && j < ir->file_count; ++j) {
            if (RP_EQ(ir->files[j].id, x->file)) f = &ir->files[j];
        }
        if (x->file.ptr && f == NULL) {
            ERR(c, x->pos, "RP1315", "file '%s' is not a [file.*] of this package", rp_cs(x->file));
        } else if (f && (f->pe_machine == 0 || f->pe_is_dll)) {
            ERR(c, x->pos, "RP1315", "'%s' is not a program (.exe)", rp_cs(f->source));
        }
        for (size_t j = 0; j < k; ++j) {
            if (x->file.ptr && ir->services[j].file.ptr && RP_EQ(ir->services[j].file, x->file)) {
                ERR(c, x->pos, "RP1316", "file '%s' already runs service '%s'", rp_cs(x->file), rp_cs(ir->services[j].id));
            }
            if (x->name.ptr && ir->services[j].name.ptr && ir_ascii_casecmp(rp_cs(ir->services[j].name), rp_cs(x->name)) == 0) {
                ERR(c, x->pos, "RP1301", "service name '%s' is already used", rp_cs(x->name));
            }
        }
    }
    // Launch conditions are keyed by their text; search properties are unique and not a
    // [property.*] of the package.
    for (size_t k = 0; k < ir->require_count; ++k) {
        for (size_t j = 0; j < k; ++j) {
            if (ir->requires[k].condition.ptr && ir->requires[j].condition.ptr && RP_EQ(ir->requires[k].condition, ir->requires[j].condition)) {
                ERR(c, ir->requires[k].pos, "RP1301", "the same condition is already required (line %u)", (unsigned)ir->requires[j].pos.line);
            }
        }
    }
    for (size_t k = 0; k < ir->search_count; ++k) {
        const rp_ir_search_t *x = &ir->searches[k];
        if (x->property.ptr == NULL) continue;
        for (size_t j = 0; j < k; ++j) {
            if (ir->searches[j].property.ptr && RP_EQ(ir->searches[j].property, x->property)) {
                ERR(c, x->pos, "RP1301", "property '%s' is already searched (line %u)", rp_cs(x->property), (unsigned)ir->searches[j].pos.line);
            }
        }
        for (size_t j = 0; j < ir->property_count; ++j) {
            if (RP_EQ(ir->properties[j].id, x->property)) ERR(c, x->pos, "RP1310", "'%s' is also a [property.*]", rp_cs(x->property));
        }
        // A search may give a dir its default (RFC-0012 V5): a folder from the registry or from a
        // folder search, never a file's path.
        if (rp_ir_find_dir(ir, rp_cs(x->property))) {
            if (x->kind != RP_SEARCH_REGISTRY && x->kind != RP_SEARCH_DIR) {
                ERR(c, x->pos, "RP1310", "'%s' is the name of a dir: only a registry or dir search can fill a dir", rp_cs(x->property));
            } else {
                ir->searches[k].fills_dir = true;
            }
        }
    }
    for (size_t k = 0; k < ir->ini_count; ++k) {
        rp_ir_ini_t *x = &ir->inis[k];
        const rp_ir_dir_t *d = x->dir.ptr ? rp_ir_find_dir(ir, rp_cs(x->dir)) : NULL;
        if (x->dir.ptr && d == NULL) {
            ERR(c, x->pos, "RP1315", "dir '%s' is not a dir ID", rp_cs(x->dir));
            continue;
        }
        if (x->feature.ptr && !find_feature(ir, rp_cs(x->feature))) {
            ERR(c, x->pos, "RP1307", "feature '%s' is not defined", rp_cs(x->feature));
        } else if (x->feature.ptr == NULL) {
            if (d && d->feature.ptr) x->feature = RP_V(ir_dup(c, rp_cs(d->feature)));
            else if (!declared) x->feature = RP_V(ir_dup(c, "Main"));
            else ERR(c, x->pos, "RP1202", "[ini.%s] needs a feature: set 'feature' here or on its dir", rp_cs(x->id));
        }
    }
    for (size_t k = 0; k < ir->merge_count; ++k) {
        rp_ir_merge_t *x = &ir->merges[k];
        if (x->dir.ptr && rp_ir_find_dir(ir, rp_cs(x->dir)) == NULL) ERR(c, x->pos, "RP1315", "dir '%s' is not a dir ID", rp_cs(x->dir));
        const rp_ir_dir_t *d = x->dir.ptr ? rp_ir_find_dir(ir, rp_cs(x->dir)) : NULL;
        if (x->feature.ptr && !find_feature(ir, rp_cs(x->feature))) {
            ERR(c, x->pos, "RP1307", "feature '%s' is not defined", rp_cs(x->feature));
        } else if (x->feature.ptr == NULL) {
            if (d && d->feature.ptr) x->feature = RP_V(ir_dup(c, rp_cs(d->feature)));
            else if (!declared) x->feature = RP_V(ir_dup(c, "Main"));
            else ERR(c, x->pos, "RP1202", "[merge.%s] needs a feature: set 'feature' here or on its dir", rp_cs(x->id));
        }
    }
    for (size_t k = 0; k < ir->env_count; ++k) {
        rp_ir_env_t *e = &ir->envs[k];
        if (e->feature.ptr && !find_feature(ir, rp_cs(e->feature))) {
            ERR(c, e->pos, "RP1307", "feature '%s' is not defined", rp_cs(e->feature));
        } else if (e->feature.ptr == NULL) {
            if (!declared) e->feature = RP_V(ir_dup(c, "Main"));
            else ERR(c, e->pos, "RP1202", "[env.%s] needs a feature", rp_cs(e->id));
        }
    }
    for (size_t k = 0; k < ir->copy_count; ++k) {
        const rp_ir_copy_t *cp = &ir->copies[k];
        bool found = false;
        for (size_t j = 0; cp->source_file.ptr && j < ir->file_count; ++j) found |= RP_EQ(ir->files[j].id, cp->source_file);
        if (cp->source_file.ptr && !found) ERR(c, cp->pos, "RP1315", "source: file '%s' is not a [file.*] of this package", rp_cs(cp->source_file));
        if (cp->dir.ptr && !rp_ir_find_dir(ir, rp_cs(cp->dir))) ERR(c, cp->pos, "RP1315", "dir '%s' is not a dir ID", rp_cs(cp->dir));
    }
    // Shortcuts: target file and folders exist.
    for (size_t k = 0; k < ir->shortcut_count; ++k) {
        const rp_ir_shortcut_t *sc = &ir->shortcuts[k];
        bool found = false;
        for (size_t j = 0; sc->target_file.ptr && j < ir->file_count; ++j) found |= RP_EQ(ir->files[j].id, sc->target_file);
        if (sc->target_file.ptr && !found) ERR(c, sc->pos, "RP1315", "target: file '%s' is not a [file.*] of this package", rp_cs(sc->target_file));
        if (sc->dir.ptr && !ir_shortcut_folder(rp_cs(sc->dir)) && !rp_ir_find_dir(ir, rp_cs(sc->dir))) {
            ERR(c, sc->pos, "RP1315", "dir '%s' is neither a dir ID nor Programs, Desktop, StartMenu or Startup", rp_cs(sc->dir));
        }
        if (sc->working_dir.ptr && !rp_ir_find_dir(ir, rp_cs(sc->working_dir))) {
            ERR(c, sc->pos, "RP1315", "working-dir '%s' is not a dir ID", rp_cs(sc->working_dir));
        }
    }
    ir_more_pe_checks(c);
    // Actions: run names an exe of this package that the package's machines can start.
    for (size_t k = 0; k < ir->action_count; ++k) {
        const rp_ir_action_t *a = &ir->actions[k];
        if (a->run_file.ptr == NULL) continue;
        const rp_ir_file_t *f = NULL;
        for (size_t j = 0; j < ir->file_count; ++j) {
            if (RP_EQ(ir->files[j].id, a->run_file)) f = &ir->files[j];
        }
        if (f == NULL) {
            ERR(c, a->pos, "RP1311", "run: file '%s' is not a [file.*] of this package", rp_cs(a->run_file));
            continue;
        }
        bool runs = f->pe_machine == RP_PE_I386 ||
                    (f->pe_machine == RP_PE_AMD64 && ir->arch != RP_ARCH_X86) ||
                    (f->pe_machine == RP_PE_ARM64 && ir->arch == RP_ARCH_ARM64);
        if (f->pe_machine == 0 || f->pe_is_dll) {
            ERR(c, a->pos, "RP1311", "run: '%s' is not a program (.exe)", rp_cs(f->source.ptr ? f->source : f->id));
        } else if (!runs) {
            ERR(c, a->pos, "RP1311", "run: '%s' (machine 0x%04X) cannot run on this package's machines", rp_cs(f->source), f->pe_machine);
        }
        for (size_t j = 0; j < k; ++j) {
            if (RP_EQ(ir->actions[j].id, a->id)) ERR(c, a->pos, "RP1301", "action '%s' is already defined", rp_cs(a->id));
        }
    }
    ir_action_checks(c);

    dup_t *same = NULL;
    size_t nsame = folded ? equal_pairs(c, (const char *const *)folded, nf, &same) : 0;
    for (size_t k = 0; k < nsame; ++k) {
        ERR(c, where[same[k].b], "RP1511", "installs to the same path as line %u (Windows ignores case in names%s)",
            (unsigned)where[same[k].a].line, c->opt->nfc ? "; --nfc made their Unicode forms the same" : "");
    }
    rp_mem_free(c->alloc, same);
    for (size_t a = 0; folded && a < nf; ++a) rp_mem_free(c->alloc, folded[a]);
    rp_mem_free(c->alloc, folded);
    rp_mem_free(c->alloc, where);
    for (size_t k = 0; k < ir->dir_count; ++k) rp_mem_free(c->alloc, dir_paths[k]);
    rp_mem_free(c->alloc, dir_paths);
}
