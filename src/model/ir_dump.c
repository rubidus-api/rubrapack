// src/model/ir_dump.c - the text form of the model (the IR goldens).

#include "ir_int.h"
#include "rubrapack/cab.h"

// ---- dump ------------------------------------------------------------------------------------

static void kv(rp_buf_t *b, const char *k, proven_u8str_view_t v) {
    rp_buf_byte(b, ' ');
    rp_buf_puts(b, k);
    rp_buf_byte(b, '=');
    rp_buf_puts(b, v.ptr ? rp_cs(v) : "-");
}


proven_err_t rp_ir_dump(const rp_ir_t *ir, proven_allocator_t alloc, uint8_t **out, size_t *len) {
    static const char *const archs[] = { "x64", "arm64", "x86" };
    char num[32];
    rp_buf_t b = rp_buf_new(alloc, (size_t)1 << 24);
    rp_buf_puts(&b, ir->module ? "module" : "package");      // [module]: a merge module (RFC-0017)
    kv(&b, "name", ir->name);
    kv(&b, "summary-name", ir->summary_name);
    kv(&b, "manufacturer", ir->manufacturer);
    kv(&b, "version", ir->version);
    kv(&b, "arch", RP_V(archs[ir->arch]));
    kv(&b, "upgrade-code", ir->upgrade_code);
    kv(&b, "product-code", ir->product_code);
    (void)proven_fmt_to(num, sizeof num, "{}", proven_arg_u32(ir->language));
    kv(&b, "language", RP_V(num));
    kv(&b, "reboot", RP_V(ir->reboot_suppress ? "suppress" : "allow"));
    if (ir->no_cleanup) kv(&b, "cleanup", PROVEN_LIT("false"));
    if (ir->no_preflight) kv(&b, "preflight", PROVEN_LIT("false"));
    if (ir->close_programs) kv(&b, "close-programs", RP_V(ir->close_programs == 1 ? "always" : "never"));
    if (ir->extra_files) kv(&b, "extra-files", RP_V(ir->extra_files == 1 ? "keep" : "delete"));
    if (ir->parent.ptr) kv(&b, "parent", ir->parent);
    if (ir->remove_addons) kv(&b, "remove-addons", PROVEN_LIT("true"));
    for (size_t i = 0; i < ir->replace_count; ++i) kv(&b, "replaces", RP_V(ir->replaces[i]));
    kv(&b, "downgrade-message", ir->downgrade_message);
    if (ir->compress >= RP_CAB_LZX(15)) (void)proven_fmt_to(num, sizeof num, "lzx:{}", proven_arg_i32(ir->compress - RP_CAB_LZX(0)));
    else (void)proven_fmt_to(num, sizeof num, "{}", proven_arg_i32(ir->compress));
    kv(&b, "compress", RP_V(ir->compress < 0 ? "none" : num));
    if (ir->scope) kv(&b, "scope", RP_V(ir->scope == 1 ? "user" : "dual"));      // only when set: older goldens stay
    if (ir->instances) { char n[16]; (void)proven_fmt_to(n, sizeof n, "{}", proven_arg_i32(ir->instances)); kv(&b, "instances", RP_V(n)); }
    if (ir->ui) {
        static const char *const sets[] = { "none", "basic", "minimal", "installdir", "features" };
        kv(&b, "ui", RP_V(sets[ir->ui]));
        kv(&b, "license", ir->license_shown);
        kv(&b, "install-dir", ir->ui_install_dir);
    }
    if (ir->ui_embedded_shown.ptr) kv(&b, "embedded-ui", ir->ui_embedded_shown);
    if (ir->cab_external || ir->cab_max) {
        kv(&b, "cab", RP_V(ir->cab_external ? "external" : "embed"));
        (void)proven_fmt_to(num, sizeof num, "{}", proven_arg_u64((unsigned long long)(ir->cab_max >> 20)));
        kv(&b, "cab-max-size", RP_V(num));
    }
    if (ir->refuse_below.ptr) {         // only when set: older goldens stay as they are
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
                const char *a = rp_cs(kind == 0 ? ir->features[idx[j - 1]].id : kind == 1 ? ir->dirs[idx[j - 1]].id : ir->files[idx[j - 1]].id);
                const char *bb = rp_cs(kind == 0 ? ir->features[idx[j]].id : kind == 1 ? ir->dirs[idx[j]].id : ir->files[idx[j]].id);
                if (ir_cmp_str(RP_V(a), RP_V(bb)) <= 0) break;
                size_t t = idx[j];
                idx[j] = idx[j - 1];
                idx[j - 1] = t;
            }
        }
        for (size_t k = 0; k < n; ++k) {
            if (kind == 0) {
                const rp_ir_feature_t *f = &ir->features[idx[k]];
                rp_buf_puts(&b, "feature ");
                rp_buf_putv(&b, f->id);
                kv(&b, "title", f->title);
                kv(&b, "description", f->description);
                (void)proven_fmt_to(num, sizeof num, "{}", proven_arg_i32(f->level));
                kv(&b, "level", RP_V(num));
                kv(&b, "hidden", RP_V(f->hidden ? "1" : "0"));
                kv(&b, "parent", f->parent);
                kv(&b, "implicit", RP_V(f->implicit ? "1" : "0"));
                if (f->required) kv(&b, "required", PROVEN_LIT("1"));
                if (f->follow_parent) kv(&b, "follow-parent", PROVEN_LIT("1"));
                if (f->no_advertise) kv(&b, "no-advertise", PROVEN_LIT("1"));
                if (f->default_when.ptr) kv(&b, "default-when", f->default_when);
                if (f->when.ptr) kv(&b, "when", f->when);
            } else if (kind == 1) {
                const rp_ir_dir_t *d = &ir->dirs[idx[k]];
                rp_buf_puts(&b, "dir ");
                rp_buf_putv(&b, d->id);
                kv(&b, "base", d->base);
                kv(&b, "parent", d->parent);
                rp_buf_puts(&b, " parts=");
                for (size_t j = 0; j < d->part_count; ++j) {
                    if (j) rp_buf_byte(&b, '/');
                    rp_buf_puts(&b, d->parts[j]);
                }
                if (d->guard) kv(&b, "guard", PROVEN_LIT("1"));
                kv(&b, "feature", d->feature);
                if (d->implicit) rp_buf_puts(&b, " implicit=1");
            } else {
                const rp_ir_file_t *f = &ir->files[idx[k]];
                rp_buf_puts(&b, "file ");
                rp_buf_putv(&b, f->id);
                kv(&b, "dir", f->dir);
                kv(&b, "source", f->source);
                kv(&b, "name", f->name);
                (void)proven_fmt_to(num, sizeof num, "{}", proven_arg_u64((unsigned long long)f->size));
                kv(&b, "size", RP_V(num));
                kv(&b, "vital", RP_V(f->vital ? "1" : "0"));
                kv(&b, "any-arch", RP_V(f->any_arch ? "1" : "0"));
                kv(&b, "feature", f->feature);
                kv(&b, "component-guid", f->component_guid);
                if (f->keep) kv(&b, "keep", PROVEN_LIT("1"));
                if (f->when.ptr) kv(&b, "when", f->when);
                if (f->isolate_for.ptr) kv(&b, "isolate-for", f->isolate_for);
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
            for (size_t j = i; j > 0 && ir_cmp_str(ir->folders[order[j - 1]].id, ir->folders[order[j]].id) > 0; --j) {
                size_t t = order[j];
                order[j] = order[j - 1];
                order[j - 1] = t;
            }
        }
        for (size_t k = 0; order && k < ir->folder_count; ++k) {
            const rp_ir_folder_t *f = &ir->folders[order[k]];
            rp_buf_puts(&b, "folder ");
            rp_buf_putv(&b, f->id);
            kv(&b, "dir", f->dir);
            kv(&b, "name", f->name);
            kv(&b, "keep", RP_V(f->keep ? "1" : "0"));
            kv(&b, "feature", f->feature);
            rp_buf_byte(&b, '\n');
        }
        rp_mem_free(alloc, order);
    }
    // RFC-0003 items, only when present (older goldens stay as they are).
    if (ir->arp_no_modify || ir->arp_no_repair || ir->arp_no_remove || ir->arp_help.ptr || ir->arp_about.ptr || ir->arp_icon_shown.ptr || ir->arp_logo_shown.ptr) {
        rp_buf_puts(&b, "arp");
        kv(&b, "no-modify", RP_V(ir->arp_no_modify ? "1" : "0"));
        kv(&b, "no-repair", RP_V(ir->arp_no_repair ? "1" : "0"));
        if (ir->arp_no_remove) kv(&b, "no-remove", PROVEN_LIT("1"));
        kv(&b, "help", ir->arp_help);
        kv(&b, "about", ir->arp_about);
        if (ir->arp_icon_shown.ptr) kv(&b, "icon", ir->arp_icon_shown);
        if (ir->arp_logo_shown.ptr) kv(&b, "logo", ir->arp_logo_shown);
        rp_buf_byte(&b, '\n');
    }
    for (size_t k = 0; k < ir->property_count; ++k) {
        const rp_ir_property_t *p = &ir->properties[k];
        rp_buf_puts(&b, "property ");
        rp_buf_putv(&b, p->id);
        kv(&b, "value", p->value);
        kv(&b, "secure", RP_V(p->secure ? "1" : "0"));
        kv(&b, "hidden", RP_V(p->hidden ? "1" : "0"));
        rp_buf_byte(&b, '\n');
    }
    for (size_t k = 0; k < ir->action_count; ++k) {
        const rp_ir_action_t *a = &ir->actions[k];
        static const char *const phases[] = { "immediate", "deferred", "commit", "rollback" };
        rp_buf_puts(&b, "action ");
        rp_buf_putv(&b, a->id);
        if (a->kind == RP_ACT_PAIR) {
            kv(&b, "run", a->run_file);
            kv(&b, "do", a->do_args);
            kv(&b, "undo", a->undo_args);
            if (a->check_args.ptr) kv(&b, "check", a->check_args);
        } else {
            if (a->run_file.ptr) kv(&b, "run", a->run_file);
            if (a->dll_file.ptr) kv(&b, "dll", a->dll_file);
            if (a->binary_shown.ptr) kv(&b, "binary", a->binary_shown);
            if (a->entry.ptr) kv(&b, "entry", a->entry);
            if (a->args.ptr) kv(&b, "args", a->args);
            kv(&b, "phase", RP_V(phases[a->phase]));
            if (a->as_user >= 0) kv(&b, "as", RP_V(a->as_user ? "user" : "system"));
            if (a->when.ptr) kv(&b, "when", a->when);
            if (a->data.ptr) kv(&b, "data", a->data);
            if (a->before.ptr) kv(&b, "before", a->before);
            if (a->after.ptr) kv(&b, "after", a->after);
            if (a->sequence) { char n[12]; (void)proven_fmt_to(n, sizeof n, "{}", proven_arg_i32((int)a->sequence)); kv(&b, "sequence", RP_V(n)); }
            if (a->ignore_exit) kv(&b, "ignore-exit", PROVEN_LIT("1"));
        }
        rp_buf_byte(&b, '\n');
    }
    static const char *const roots[] = { "HKMU", "HKCR", "HKCU", "HKLM" };     // index root + 1
    static const char *const rtypes[] = { "string", "expand", "dword", "binary", "multi", "qword" };
    for (size_t k = 0; k < ir->registry_count; ++k) {
        const rp_ir_registry_t *r = &ir->registries[k];
        rp_buf_puts(&b, "registry ");
        rp_buf_putv(&b, r->id);
        kv(&b, "root", RP_V(roots[r->root + 1]));
        kv(&b, "key", r->key);
        kv(&b, "name", r->name);
        kv(&b, "type", RP_V(rtypes[r->type]));
        kv(&b, "value", r->value);
        for (size_t j = 0; j < r->item_count; ++j) kv(&b, "item", RP_V(r->items[j]));
        kv(&b, "remove", RP_V(r->remove ? "1" : "0"));
        if (r->purge) kv(&b, "on", PROVEN_LIT("uninstall"));
        if (r->users_all) kv(&b, "users", PROVEN_LIT("all"));
        kv(&b, "keep", RP_V(r->keep ? "1" : "0"));
        kv(&b, "view", RP_V(r->view32 ? "32" : "native"));
        kv(&b, "with", r->with_file);
        kv(&b, "feature", r->feature);
        if (r->when.ptr) kv(&b, "when", r->when);
        rp_buf_byte(&b, '\n');
    }
    static const char *const modes[] = { "-", "install", "uninstall", "both" };
    for (size_t k = 0; k < ir->remove_count; ++k) {
        const rp_ir_remove_t *r = &ir->removes[k];
        rp_buf_puts(&b, "remove ");
        rp_buf_putv(&b, r->id);
        kv(&b, "dir", r->dir);
        kv(&b, "name", r->name);
        kv(&b, "on", RP_V(modes[r->mode & 3]));
        if (r->keep_on_upgrade) kv(&b, "upgrade", PROVEN_LIT("false"));
        if (r->recursive) kv(&b, "recursive", PROVEN_LIT("true"));
        if (r->users_all) kv(&b, "users", PROVEN_LIT("all"));
        kv(&b, "feature", r->feature);
        rp_buf_byte(&b, '\n');
    }
    for (size_t k = 0; k < ir->require_count; ++k) {
        const rp_ir_require_t *r = &ir->requires[k];
        rp_buf_puts(&b, "require ");
        rp_buf_putv(&b, r->id);
        kv(&b, "condition", r->condition);
        kv(&b, "message", r->message);
        rp_buf_byte(&b, '\n');
    }
    static const char *const pkinds[] = { "dir", "file", "registry" };
    for (size_t k = 0; k < ir->permission_count; ++k) {
        const rp_ir_permission_t *x = &ir->permissions[k];
        rp_buf_puts(&b, "permission ");
        rp_buf_putv(&b, x->id);
        kv(&b, "kind", RP_V(x->kind >= 0 && x->kind < 3 ? pkinds[x->kind] : "-"));
        kv(&b, "target", x->target);
        kv(&b, "sddl", x->sddl);
        kv(&b, "feature", x->feature);
        rp_buf_byte(&b, '\n');
    }
    char lkey[32];
#define KV_LANG(base, lang, val)                                     \
    do {                                                             \
        (void)proven_fmt_to(lkey, sizeof lkey, "{}-{}", proven_arg_cstr((base)), proven_arg_cstr(rp_cs(lang))); \
        kv(&b, lkey, RP_V(val));                              \
    } while (0)
    for (size_t k = 1; k < ir->ui_lang_count; ++k) {      // RFC-0012: English (0) is always there
        const rp_ir_ui_lang_t *L = &ir->ui_langs[k];
        char ids[RP_UI_LANGID_MAX * 6 + 1] = "";
        for (size_t j = 0; j < L->langid_count; ++j) {
            size_t n = strlen(ids);
            (void)proven_fmt_to(ids + n, sizeof ids - n, "{}{}", proven_arg_cstr(j ? "," : ""), proven_arg_u32((unsigned)L->langids[j]));
        }
        rp_buf_puts(&b, "ui-language ");
        rp_buf_puts(&b, L->code);
        kv(&b, "name", L->name);
        kv(&b, "font", L->font);
        kv(&b, "langids", RP_V(ids));
        kv(&b, "license", L->license_shown);
        rp_buf_byte(&b, '\n');
    }
    for (size_t k = 0; k < ir->ui_text_count; ++k) {
        rp_buf_puts(&b, "ui-text ");
        rp_buf_putv(&b, ir->ui_texts[k].id);
        kv(&b, "text", ir->ui_texts[k].text);
        for (size_t j = 0; j < ir->ui_texts[k].by_lang_count; ++j) KV_LANG("text", ir->ui_texts[k].by_lang[j].lang, ir->ui_texts[k].by_lang[j].text);
        rp_buf_byte(&b, '\n');
    }
    for (size_t k = 0; k < ir->dialog_count; ++k) {
        rp_buf_puts(&b, "dialog ");
        rp_buf_putv(&b, ir->dialogs[k].id);
        kv(&b, "title", ir->dialogs[k].title);
        kv(&b, "description", ir->dialogs[k].description);
        kv(&b, "after", ir->dialogs[k].after);
        for (size_t j = 0; j < ir->dialogs[k].title_by_lang_count; ++j) KV_LANG("title", ir->dialogs[k].title_by_lang[j].lang, ir->dialogs[k].title_by_lang[j].text);
        for (size_t j = 0; j < ir->dialogs[k].description_by_lang_count; ++j)
            KV_LANG("description", ir->dialogs[k].description_by_lang[j].lang, ir->dialogs[k].description_by_lang[j].text);
        rp_buf_byte(&b, '\n');
    }
    for (size_t k = 0; k < ir->dialog_control_count; ++k) {
        static const char *const types[] = { "text", "checkbox", "edit", "radio", "combo", "listbox", "bitmap" };
        const rp_ir_dialog_control_t *x = &ir->dialog_controls[k];
        char geo[64];
        rp_buf_puts(&b, "dialog-control ");
        rp_buf_putv(&b, x->id);
        kv(&b, "dialog", x->dialog);
        kv(&b, "type", RP_V(x->type >= 0 && x->type < (int)(sizeof types / sizeof *types) ? types[x->type] : "-"));
        (void)proven_fmt_to(geo, sizeof geo, "{},{},{},{}", proven_arg_i32(x->x), proven_arg_i32(x->y), proven_arg_i32(x->width), proven_arg_i32(x->height));
        kv(&b, "at", RP_V(geo));
        kv(&b, "text", x->text);
        kv(&b, "property", x->property);
        if (x->image_shown.ptr) kv(&b, "image", x->image_shown);
        for (size_t j = 0; j < x->value_count; ++j) {
            kv(&b, "value", RP_V(x->values[j]));
            kv(&b, "label", RP_V(x->labels[j]));
        }
        for (size_t j = 0; j < x->text_by_lang_count; ++j) KV_LANG("text", x->text_by_lang[j].lang, x->text_by_lang[j].text);
        for (size_t j = 0; j < x->labels_by_lang_count; ++j) {
            for (size_t v = 0; v < x->value_count; ++v) KV_LANG("label", x->labels_by_lang[j].lang, x->labels_by_lang[j].labels[v]);
        }
        rp_buf_byte(&b, '\n');
    }
    for (size_t k = 0; k < ir->billboard_count; ++k) {
        const rp_ir_billboard_t *x = &ir->billboards[k];
        char num[16];
        rp_buf_puts(&b, "billboard ");
        rp_buf_putv(&b, x->id);
        kv(&b, "image", x->image_shown);
        kv(&b, "feature", x->feature);
        if (x->order) { (void)proven_fmt_to(num, sizeof num, "{}", proven_arg_i32(x->order)); kv(&b, "order", RP_V(num)); }
        rp_buf_byte(&b, '\n');
    }
    for (size_t k = 0; k < ir->font_count; ++k) {
        rp_buf_puts(&b, "font ");
        rp_buf_putv(&b, ir->fonts[k].id);
        kv(&b, "file", ir->fonts[k].file);
        kv(&b, "title", ir->fonts[k].title);
        rp_buf_byte(&b, '\n');
    }
    static const char *const starts[] = { "-", "-", "auto", "demand", "disabled" };
    static const char *const accounts[] = { "LocalSystem", "LocalService", "NetworkService" };
    for (size_t k = 0; k < ir->service_count; ++k) {
        const rp_ir_service_t *x = &ir->services[k];
        rp_buf_puts(&b, "service ");
        rp_buf_putv(&b, x->id);
        kv(&b, "file", x->file);
        kv(&b, "name", x->name);
        kv(&b, "display-name", x->display_name);
        kv(&b, "description", x->description);
        kv(&b, "start", RP_V(starts[x->start]));
        kv(&b, "account", RP_V(accounts[x->account]));
        kv(&b, "args", x->args);
        kv(&b, "start-on-install", RP_V(x->start_on_install ? "1" : "0"));
        rp_buf_byte(&b, '\n');
    }
    static const char *const skinds[] = { "registry", "file", "dir", "component" };
    static const char *const sroots[] = { "HKCR", "HKCU", "HKLM" };
    for (size_t k = 0; k < ir->search_count; ++k) {
        const rp_ir_search_t *x = &ir->searches[k];
        rp_buf_puts(&b, "search ");
        rp_buf_putv(&b, x->id);
        kv(&b, "property", x->property);
        kv(&b, "kind", RP_V(skinds[x->kind]));
        if (x->kind == RP_SEARCH_REGISTRY) {
            kv(&b, "root", RP_V(sroots[x->root]));
            kv(&b, "key", x->key);
            kv(&b, "name", x->name);
            kv(&b, "view", RP_V(x->view32 ? "32" : "native"));
        }
        kv(&b, "base", x->base);
        kv(&b, "path", x->path);
        kv(&b, "file", x->file_name);
        kv(&b, "min-version", x->min_version);
        kv(&b, "component-guid", x->component_guid);
        if (x->fills_dir) kv(&b, "fills-dir", PROVEN_LIT("1"));
        rp_buf_byte(&b, '\n');
    }
    static const char *const imodes[] = { "set", "add", "remove" };
    for (size_t k = 0; k < ir->ini_count; ++k) {
        const rp_ir_ini_t *x = &ir->inis[k];
        rp_buf_puts(&b, "ini ");
        rp_buf_putv(&b, x->id);
        kv(&b, "dir", x->dir);
        kv(&b, "file", x->file);
        kv(&b, "section", x->section);
        kv(&b, "key", x->key);
        kv(&b, "value", x->value);
        kv(&b, "mode", RP_V(imodes[x->mode]));
        kv(&b, "feature", x->feature);
        if (x->when.ptr) kv(&b, "when", x->when);
        rp_buf_byte(&b, '\n');
    }
    static const char *const emodes[] = { "set", "append", "prepend" };
    for (size_t k = 0; k < ir->env_count; ++k) {
        const rp_ir_env_t *e = &ir->envs[k];
        rp_buf_puts(&b, "env ");
        rp_buf_putv(&b, e->id);
        kv(&b, "name", e->name);
        kv(&b, "value", e->value);
        kv(&b, "mode", RP_V(emodes[e->mode]));
        kv(&b, "keep", RP_V(e->keep ? "1" : "0"));
        kv(&b, "feature", e->feature);
        if (e->when.ptr) kv(&b, "when", e->when);
        rp_buf_byte(&b, '\n');
    }
    for (size_t k = 0; k < ir->copy_count; ++k) {
        const rp_ir_copy_t *cp = &ir->copies[k];
        rp_buf_puts(&b, "copy ");
        rp_buf_putv(&b, cp->id);
        kv(&b, "source", cp->source_file);
        kv(&b, "dir", cp->dir);
        kv(&b, "name", cp->name);
        rp_buf_byte(&b, '\n');
    }
    for (size_t k = 0; k < ir->shortcut_count; ++k) {
        const rp_ir_shortcut_t *sc = &ir->shortcuts[k];
        rp_buf_puts(&b, "shortcut ");
        rp_buf_putv(&b, sc->id);
        kv(&b, "dir", sc->dir);
        kv(&b, "name", sc->name);
        kv(&b, "target", sc->target_file);
        kv(&b, "args", sc->args);
        kv(&b, "description", sc->description);
        kv(&b, "working-dir", sc->working_dir);
        if (sc->icon_shown.ptr) kv(&b, "icon", sc->icon_shown);
        if (sc->when.ptr) kv(&b, "when", sc->when);
        if (sc->advertise) kv(&b, "advertise", PROVEN_LIT("1"));
        rp_buf_byte(&b, '\n');
    }
    for (size_t k = 0; k < ir->menu_count; ++k) {
        const rp_ir_menu_t *x = &ir->menus[k];
        static const char *const multi[] = { "each", "one", "single" };
        rp_buf_puts(&b, "menu ");
        rp_buf_putv(&b, x->id);
        for (size_t j = 0; j < x->on_count; ++j) kv(&b, "on", RP_V(x->on[j]));
        kv(&b, "text", x->text);
        for (size_t j = 0; j < x->text_by_lang_count; ++j) {
            kv(&b, "text-lang", x->text_by_lang[j].lang);
            kv(&b, "text", x->text_by_lang[j].text);
        }
        kv(&b, "target", x->target_file);
        kv(&b, "icon", x->icon_file);
        kv(&b, "args", x->args);
        if (x->list.ptr) kv(&b, "list", x->list);
        if (x->hide_when.ptr) kv(&b, "hide-when", x->hide_when);
        kv(&b, "parent", x->parent);
        kv(&b, "class", x->clsid);
        kv(&b, "multi", RP_V(multi[x->multi]));
        if (x->extended) kv(&b, "extended", PROVEN_LIT("1"));
        if (!x->windows11) kv(&b, "windows11", PROVEN_LIT("0"));
        rp_buf_byte(&b, '\n');
    }
    for (size_t k = 0; k < ir->move_count; ++k) {
        const rp_ir_move_t *x = &ir->moves[k];
        rp_buf_puts(&b, "move ");
        rp_buf_putv(&b, x->id);
        if (x->from_dir.ptr) kv(&b, "from", x->from_dir);
        if (x->from_prop.ptr) kv(&b, "from-prop", x->from_prop);
        kv(&b, "name", x->name);
        kv(&b, "to", x->to_dir);
        if (x->new_name.ptr) kv(&b, "new-name", x->new_name);
        kv(&b, "copy", RP_V(x->copy ? "1" : "0"));
        if (x->with_file.ptr) kv(&b, "with", x->with_file);
        if (x->feature.ptr) kv(&b, "feature", x->feature);
        rp_buf_byte(&b, '\n');
    }
    for (size_t k = 0; k < ir->odbc_driver_count; ++k) {
        const rp_ir_odbc_driver_t *x = &ir->odbc_drivers[k];
        rp_buf_puts(&b, "odbc-driver ");
        rp_buf_putv(&b, x->id);
        kv(&b, "file", x->file);
        if (x->setup.ptr) kv(&b, "setup", x->setup);
        kv(&b, "description", x->description);
        for (size_t j = 0; j < x->attribute_count; ++j) kv(&b, "attribute", RP_V(x->attributes[j]));
        rp_buf_byte(&b, '\n');
    }
    for (size_t k = 0; k < ir->odbc_source_count; ++k) {
        const rp_ir_odbc_source_t *x = &ir->odbc_sources[k];
        rp_buf_puts(&b, "odbc-source ");
        rp_buf_putv(&b, x->id);
        if (x->driver.ptr) kv(&b, "driver", x->driver);
        if (x->driver_name.ptr) kv(&b, "driver-name", x->driver_name);
        kv(&b, "name", x->name);
        kv(&b, "registration", RP_V(x->user ? "user" : "machine"));
        for (size_t j = 0; j < x->attribute_count; ++j) kv(&b, "attribute", RP_V(x->attributes[j]));
        if (x->with_file.ptr) kv(&b, "with", x->with_file);
        rp_buf_byte(&b, '\n');
    }
    for (size_t k = 0; k < ir->publish_count; ++k) {
        const rp_ir_publish_t *x = &ir->publishes[k];
        rp_buf_puts(&b, "publish ");
        rp_buf_putv(&b, x->id);
        kv(&b, "file", x->file);
        kv(&b, "category", x->category);
        kv(&b, "qualifier", x->qualifier);
        if (x->data.ptr) kv(&b, "data", x->data);
        rp_buf_byte(&b, '\n');
    }
    for (size_t k = 0; k < ir->other_count; ++k) {
        const rp_ir_other_t *x = &ir->others[k];
        rp_buf_puts(&b, "other-copy ");
        rp_buf_putv(&b, x->id);
        if (x->msix.ptr) kv(&b, "msix", x->msix);
        if (x->publisher.ptr) kv(&b, "publisher", x->publisher);
        if (x->upgrade_code.ptr) kv(&b, "upgrade-code", x->upgrade_code);
        kv(&b, "action", RP_V(x->refuse ? "refuse" : "warn"));
        if (x->message.ptr) kv(&b, "message", x->message);
        for (size_t j = 0; j < x->message_by_lang_count; ++j) {
            kv(&b, "message-lang", x->message_by_lang[j].lang);
            kv(&b, "message", x->message_by_lang[j].text);
        }
        rp_buf_byte(&b, '\n');
    }
    for (size_t k = 0; k < ir->assoc_count; ++k) {
        const rp_ir_assoc_t *x = &ir->assocs[k];
        rp_buf_puts(&b, "assoc ");
        rp_buf_putv(&b, x->id);
        kv(&b, "extension", x->extension);
        kv(&b, "prog-id", x->prog_id);
        kv(&b, "description", x->description);
        kv(&b, "target", x->target_file);
        kv(&b, "icon", x->icon_file);
        if (x->logo.ptr) kv(&b, "logo", x->logo);
        kv(&b, "args", x->args);
        if (x->content_type.ptr) kv(&b, "content-type", x->content_type);
        if (x->perceived_type.ptr) kv(&b, "perceived-type", x->perceived_type);
        if (x->no_default) kv(&b, "default", PROVEN_LIT("0"));
        rp_buf_byte(&b, '\n');
    }
    for (size_t k = 0; k < ir->com_count; ++k) {
        const rp_ir_com_t *x = &ir->coms[k];
        rp_buf_puts(&b, "com ");
        rp_buf_putv(&b, x->id);
        kv(&b, "file", x->file);
        kv(&b, "class", x->clsid);
        kv(&b, "description", x->description);
        kv(&b, "threading", x->threading);
        kv(&b, "args", x->args);
        kv(&b, "prog-id", x->prog_id);
        kv(&b, "app-id", x->app_id);
        if (x->surrogate) rp_buf_puts(&b, " surrogate");
        kv(&b, "typelib", x->typelib);
        kv(&b, "typelib-version", x->typelib_version);
        kv(&b, "typelib-file", x->typelib_file);
        rp_buf_byte(&b, '\n');
    }
    for (size_t k = 0; k < ir->protocol_count; ++k) {
        const rp_ir_protocol_t *x = &ir->protocols[k];
        rp_buf_puts(&b, "protocol ");
        rp_buf_putv(&b, x->id);
        kv(&b, "name", x->name);
        kv(&b, "description", x->description);
        kv(&b, "target", x->target_file);
        kv(&b, "args", x->args);
        rp_buf_byte(&b, '\n');
    }
    for (size_t k = 0; k < ir->msix_ext_count; ++k) {
        const rp_ir_msix_ext_t *x = &ir->msix_exts[k];
        rp_buf_puts(&b, "msix-extension ");
        rp_buf_putv(&b, x->id);
        static const char *const kinds[] = { "alias", "startup-task", "firewall", "com-server", "toast", "context-menu", "app-extension",
                                             "app-extension-host", "app-service", "share-target", "autoplay" };
        kv(&b, "kind", RP_V(kinds[x->kind]));
        kv(&b, "app", x->app);
        kv(&b, "alias", x->alias);
        kv(&b, "task-id", x->task_id);
        kv(&b, "display-name", x->display);
        if (x->kind == RP_MSIX_EXT_STARTUP) kv(&b, "enabled", RP_V(x->enabled ? "true" : "false"));
        rp_buf_byte(&b, '\n');
    }
    return rp_buf_take(&b, out, len);
}
