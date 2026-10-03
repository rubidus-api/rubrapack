// src/model/ir_check.c - checks across tables.

#include "ir_int.h"

// ---- cross checks --------------------------------------------------------------------------

const rp_ir_dir_t *ir_find_dir(const rp_ir_t *ir, const char *id) {
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
        head = ir_dup(c, d->base);
    } else {
        const rp_ir_dir_t *p = d->parent ? ir_find_dir(c->ir, d->parent) : NULL;
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
    r->id = ir_dup(c, rid);
    r->root = RP_ROOT_HKCR;
    r->key = ir_dup(c, key);
    r->name = name ? ir_dup(c, name) : NULL;
    r->type = RP_REG_STRING;
    r->value = ir_dup(c, value);
    r->msi_only = true;             // an MSIX says the same in its manifest (RFC-0010 N4)
    r->with_file = ir_dup(c, with);
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
        if (ir->coms[k].clsid && strcmp(ir->coms[k].clsid, clsid) == 0) return &ir->coms[k];
    }
    return NULL;
}

// [assoc.*], [protocol.*] and [msix-extension.*] (RFC-0010 N4): programs that exist, one handler per
// extension and scheme, one description per prog-id; then, for the MSI, their HKCR values in the
// program's component. HKCR follows the installation: HKLM\Software\Classes per machine,
// HKCU\Software\Classes per user.
void ir_class_checks(ctx_t *c) {
    rp_ir_t *ir = c->ir;
    if (ir->ui_launch_file) program_ok(c, ir->ui_launch_file, (rp_pos_t){ 1, 1 }, "[ui] launch");
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
                (ir_cmp_str(x->description, y->description) != 0 || ir_cmp_str(x->target_file, y->target_file) != 0 ||
                 ir_cmp_str(x->icon_file, y->icon_file) != 0 || ir_cmp_str(x->args, y->args) != 0)) {
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
            if (x->kind == RP_MSIX_EXT_ALIAS && y->kind == RP_MSIX_EXT_ALIAS && ir_cmp_str(x->app, y->app) == 0) {
                ERR(c, x->pos, "RP1301", "an application has one alias ([msix-extension.%s] is one already)", y->id);
            }
            if (x->kind == RP_MSIX_EXT_STARTUP && y->kind == RP_MSIX_EXT_STARTUP && strcmp(x->task_id, y->task_id) == 0) {
                ERR(c, x->pos, "RP1301", "task-id '%s' is already used by [msix-extension.%s]", x->task_id, y->id);
            }
        }
    }
    // [com.*] (RFC-0022): a served class of this package, each class and prog-id once.
    for (size_t k = 0; k < ir->com_count; ++k) {
        rp_ir_com_t *x = &ir->coms[k];
        const rp_ir_file_t *f = file_by_id(ir, x->file);
        const char *n = f && f->name ? f->name : "";
        size_t l = strlen(n);
        bool exe = l >= 4 && (strcmp(n + l - 4, ".exe") == 0 || strcmp(n + l - 4, ".EXE") == 0);
        bool dll = l >= 4 && (strcmp(n + l - 4, ".dll") == 0 || strcmp(n + l - 4, ".DLL") == 0);
        if (x->file && f == NULL) ERR(c, x->pos, "RP1315", "file: '%s' is not a [file.*] of this package", x->file);
        else if (f && !exe && !dll) ERR(c, x->pos, "RP1316", "file: '%s' serves COM classes only as an .exe or a .dll", x->file);
        x->exe = exe;
        if (exe && x->threading) ERR(c, x->pos, "RP1316", "[com.%s]: threading is for a DLL; a program sets its own apartment", x->id);
        if (exe && x->surrogate) ERR(c, x->pos, "RP1316", "[com.%s]: surrogate runs a DLL in dllhost; '%s' is a program", x->id, x->file);
        if (dll && x->args) ERR(c, x->pos, "RP1316", "[com.%s]: args are a program's; a DLL is loaded, not started", x->id);
        if (x->typelib_file && file_by_id(ir, x->typelib_file) == NULL) {
            ERR(c, x->pos, "RP1315", "typelib-file: '%s' is not a [file.*] of this package", x->typelib_file);
        }
        for (size_t j = 0; j < k; ++j) {
            const rp_ir_com_t *y = &ir->coms[j];
            if (x->clsid && y->clsid && strcmp(x->clsid, y->clsid) == 0) ERR(c, x->pos, "RP1301", "class %s is already served by [com.%s]", x->clsid, y->id);
            if (x->prog_id && y->prog_id && strcmp(x->prog_id, y->prog_id) == 0) ERR(c, x->pos, "RP1301", "prog-id '%s' is already [com.%s]'s", x->prog_id, y->id);
        }
        for (size_t j = 0; j < ir->msix_ext_count; ++j) {
            const rp_ir_msix_ext_t *y = &ir->msix_exts[j];
            if (x->clsid && y->clsid && strcmp(x->clsid, y->clsid) == 0) ERR(c, x->pos, "RP1301", "class %s is also in [msix-extension.%s]", x->clsid, y->id);
        }
        for (size_t j = 0; x->prog_id && j < ir->assoc_count; ++j) {
            if (ir->assocs[j].prog_id && strcmp(ir->assocs[j].prog_id, x->prog_id) == 0) {
                ERR(c, x->pos, "RP1301", "prog-id '%s' is also [assoc.%s]'s: a file type and a COM class need two names", x->prog_id, ir->assocs[j].id);
            }
        }
    }
    // [handler.*] (RFC-0024): a DLL class of a [com.*], one handler of a kind per file type.
    for (size_t k = 0; k < ir->handler_count; ++k) {
        const rp_ir_handler_t *x = &ir->handlers[k];
        const rp_ir_com_t *cm = com_by_class(ir, x->clsid);
        static const char *const kinds[] = { "preview", "thumbnail", "property" };
        if (x->clsid && cm == NULL) {
            ERR(c, x->pos, "RP1315", "[handler.%s]: class %s is not a [com.*] of this package (the handler's DLL serves it)", x->id, x->clsid);
        } else if (cm && cm->exe) {
            ERR(c, x->pos, "RP1316", "[handler.%s]: Explorer loads a %s handler from a DLL; [com.%s] is a program", x->id, kinds[x->kind], cm->id);
        } else if (cm && x->kind == RP_HANDLER_PREVIEW && (cm->app_id || cm->surrogate)) {
            ERR(c, x->pos, "RP1316", "[handler.%s]: a preview handler runs in Windows' prevhost; leave app-id and surrogate out of [com.%s]", x->id, cm->id);
        }
        if (x->kind == RP_HANDLER_PROPERTY && ir->scope != 0) {
            ERR(c, x->pos, "RP1316", "[handler.%s]: Windows reads property handlers per machine only; the package needs scope = \"machine\"", x->id);
        }
        for (size_t j = 0; j < k; ++j) {
            const rp_ir_handler_t *y = &ir->handlers[j];
            if (y->kind != x->kind) continue;
            for (size_t a = 0; a < x->type_count; ++a) {
                for (size_t b = 0; b < y->type_count; ++b) {
                    if (x->types[a] && y->types[b] && strcmp(x->types[a], y->types[b]) == 0) {
                        ERR(c, x->pos, "RP1301", "'%s' already has a %s handler ([handler.%s])", x->types[a], kinds[x->kind], y->id);
                    }
                }
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
    // [handler.*]: Explorer finds a handler under the file type's ShellEx key (preview, thumbnail),
    // or in PropertySystem\PropertyHandlers (property, HKLM); a preview handler is also on the
    // PreviewHandlers list and runs in prevhost (its class's AppID). All in the DLL's component.
    for (size_t k = 0; k < ir->handler_count && !c->nomem; ++k) {
        const rp_ir_handler_t *x = &ir->handlers[k];
        const rp_ir_com_t *cm = com_by_class(ir, x->clsid);
        const char *desc = x->description ? x->description : cm->description ? cm->description : ir->name;
        char key[256], suffix[24];
        for (size_t t = 0; t < x->type_count; ++t) {
            snprintf(suffix, sizeof suffix, "T%zu", t + 1);
            if (x->kind == RP_HANDLER_PROPERTY) {
                snprintf(key, sizeof key, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\PropertySystem\\PropertyHandlers\\%s", x->types[t]);
                add_root_value(c, x->id, suffix, RP_ROOT_HKLM, key, NULL, x->clsid, cm->file, x->pos);
            } else {
                snprintf(key, sizeof key, "%s\\ShellEx\\%s", x->types[t],
                         x->kind == RP_HANDLER_PREVIEW ? "{8895B1C6-B41F-4C1C-A562-0D564250836F}" : "{E357FCCD-A995-4576-B01F-234630154E96}");
                add_class_value(c, x->id, suffix, key, NULL, x->clsid, cm->file, x->pos);
            }
        }
        if (x->kind == RP_HANDLER_PREVIEW) {
            snprintf(key, sizeof key, "CLSID\\%s", x->clsid);
            // prevhost.exe: the 64-bit one for an x64 or Arm64 package, the 32-bit one for x86.
            add_class_value(c, x->id, "App", key, "AppID",
                            ir->arch == RP_ARCH_X86 ? "{534A1E02-D58F-44F0-B58B-36CBED287C7C}" : "{6D2B5079-2F0B-48DD-AB7F-97CEC514D30B}", cm->file, x->pos);
            add_class_value(c, x->id, "Name", key, "DisplayName", desc, cm->file, x->pos);
            add_root_value(c, x->id, "List", RP_ROOT_HKMU, "Software\\Microsoft\\Windows\\CurrentVersion\\PreviewHandlers", x->clsid, desc,
                           cm->file, x->pos);
        }
    }
    // [com.*]: HKCR values for the MSI in the server's component, and a com-server extension for the MSIX.
    for (size_t k = 0; k < ir->com_count && !c->nomem; ++k) {
        const rp_ir_com_t *x = &ir->coms[k];
        const char *desc = x->description ? x->description : ir->name;
        char key[200], val[512];
        snprintf(key, sizeof key, "CLSID\\%s", x->clsid);
        add_class_value(c, x->id, "Cls", key, NULL, desc, x->file, x->pos);
        if (x->exe) {
            snprintf(key, sizeof key, "CLSID\\%s\\LocalServer32", x->clsid);
            char *cmd = rp_mem_alloc(c->alloc, strlen(x->file) + (x->args ? strlen(x->args) : 0) + 16, 1);
            if (cmd == NULL) {
                c->nomem = true;
                return;
            }
            sprintf(cmd, x->args ? "\"[#%s]\" %s" : "\"[#%s]\"", x->file, x->args);
            add_class_value(c, x->id, "Srv", key, NULL, cmd, x->file, x->pos);
            rp_mem_free(c->alloc, cmd);
        } else {
            snprintf(key, sizeof key, "CLSID\\%s\\InprocServer32", x->clsid);
            snprintf(val, sizeof val, "[#%s]", x->file);
            add_class_value(c, x->id, "Srv", key, NULL, val, x->file, x->pos);
            add_class_value(c, x->id, "Thr", key, "ThreadingModel", x->threading ? x->threading : "Apartment", x->file, x->pos);
        }
        if (x->prog_id) {
            snprintf(key, sizeof key, "CLSID\\%s\\ProgID", x->clsid);
            add_class_value(c, x->id, "PidC", key, NULL, x->prog_id, x->file, x->pos);
            add_class_value(c, x->id, "Pid", x->prog_id, NULL, desc, x->file, x->pos);
            snprintf(key, sizeof key, "%s\\CLSID", x->prog_id);
            add_class_value(c, x->id, "PidK", key, NULL, x->clsid, x->file, x->pos);
        }
        const char *app = x->app_id ? x->app_id : x->surrogate ? x->clsid : NULL;
        if (app) {
            snprintf(key, sizeof key, "CLSID\\%s", x->clsid);
            add_class_value(c, x->id, "App", key, "AppID", app, x->file, x->pos);
            snprintf(key, sizeof key, "AppID\\%s", app);
            add_class_value(c, x->id, "AppK", key, NULL, desc, x->file, x->pos);
            if (x->surrogate) add_class_value(c, x->id, "Sur", key, "DllSurrogate", "", x->file, x->pos);
        }
        if (x->typelib) {
            const char *tf = x->typelib_file ? x->typelib_file : x->file;
            snprintf(key, sizeof key, "CLSID\\%s\\TypeLib", x->clsid);
            add_class_value(c, x->id, "Tlb", key, NULL, x->typelib, x->file, x->pos);
            snprintf(key, sizeof key, "TypeLib\\%s\\%s", x->typelib, x->typelib_version);
            add_class_value(c, x->id, "TlbV", key, NULL, desc, tf, x->pos);
            snprintf(key, sizeof key, "TypeLib\\%s\\%s\\0\\%s", x->typelib, x->typelib_version, ir->arch == RP_ARCH_X86 ? "win32" : "win64");
            snprintf(val, sizeof val, "[#%s]", tf);
            add_class_value(c, x->id, "TlbP", key, NULL, val, tf, x->pos);
            snprintf(key, sizeof key, "TypeLib\\%s\\%s\\FLAGS", x->typelib, x->typelib_version);
            add_class_value(c, x->id, "TlbF", key, NULL, "0", tf, x->pos);
            snprintf(key, sizeof key, "TypeLib\\%s\\%s\\HELPDIR", x->typelib, x->typelib_version);
            const rp_ir_file_t *tff = file_by_id(ir, tf);
            snprintf(val, sizeof val, "[%s]", tff ? tff->dir : "INSTALLDIR");
            add_class_value(c, x->id, "TlbH", key, NULL, val, tf, x->pos);
        }
        if (x->msi_only) continue;
        rp_ir_msix_ext_t *e = &ir->msix_exts[ir->msix_ext_count++];
        memset(e, 0, sizeof *e);
        e->id = ir_dup(c, x->id);
        e->kind = RP_MSIX_EXT_COM;
        e->file = ir_dup(c, x->file);
        e->clsid = ir_dup(c, x->clsid);
        e->display = ir_dup(c, desc);
        static const char *const msix_models[][2] = { { "Apartment", "STA" }, { "Free", "MTA" }, { "Both", "Both" }, { "Neutral", "Neutral" } };
        for (size_t m = 0; m < 4; ++m) {
            if (strcmp(x->threading ? x->threading : "Apartment", msix_models[m][0]) == 0) e->threading = ir_dup(c, msix_models[m][1]);
        }
        if (x->args) e->args = ir_dup(c, x->args);
        if (x->prog_id) e->prog_id = ir_dup(c, x->prog_id);
        e->pos = x->pos;
    }
}

void ir_cross_checks(ctx_t *c) {
    rp_ir_t *ir = c->ir;
    // [arp] no-remove leaves Modify as the way to remove the product: it needs dialogs and Modify.
    if (ir->arp_no_remove && (ir->arp_no_modify || ir->ui < RP_UI_MINIMAL))
        ERR(c, ir->arp_no_remove_pos, "RP1316",
            "no-remove leaves Modify (the dialogs' Remove) as the only way to remove the product: it needs [package] ui = \"minimal\", \"installdir\" or \"features\", and no no-modify");
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
        if (d->parent && !ir_find_dir(ir, d->parent)) {
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
        if (folded[nf]) ir_fold(dir_paths[k], folded[nf], strlen(dir_paths[k]) * 2 + 4);
        where[nf++] = ir->dirs[k].pos;
    }
    for (size_t k = 0; k < ir->file_count; ++k) {
        rp_ir_file_t *f = &ir->files[k];
        const rp_ir_dir_t *d = f->dir ? ir_find_dir(ir, f->dir) : NULL;
        if (f->dir && d == NULL) {
            ERR(c, f->pos, "RP1307", "dir '%s' is not defined", f->dir);
            continue;
        }
        if (f->feature && !find_feature(ir, f->feature)) {
            ERR(c, f->pos, "RP1307", "feature '%s' is not defined", f->feature);
        } else if (f->feature == NULL) {
            if (d && d->feature) f->feature = ir_dup(c, d->feature);
            else if (!declared) f->feature = ir_dup(c, "Main");
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
                    if (folded[nf]) ir_fold(full, folded[nf], len * 2 + 4);
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
        const rp_ir_dir_t *d = f->dir ? ir_find_dir(ir, f->dir) : NULL;
        if (f->dir && d == NULL) {
            ERR(c, f->pos, "RP1307", "dir '%s' is not defined", f->dir);
            continue;
        }
        if (f->feature && !find_feature(ir, f->feature)) {
            ERR(c, f->pos, "RP1307", "feature '%s' is not defined", f->feature);
        } else if (f->feature == NULL) {
            if (d && d->feature) f->feature = ir_dup(c, d->feature);
            else if (!declared) f->feature = ir_dup(c, "Main");
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
        if (ir_find_dir(ir, p->id) || find_feature(ir, p->id)) {
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
            if (!declared) r->feature = ir_dup(c, "Main");
            else ERR(c, r->pos, "RP1202", "registry value '%s' needs a feature", r->id);
        }
    }
    // Removals and copies: folders and files exist; a removal's feature resolves like a file's.
    for (size_t k = 0; k < ir->remove_count; ++k) {
        rp_ir_remove_t *r = &ir->removes[k];
        const rp_ir_dir_t *d = r->dir ? ir_find_dir(ir, r->dir) : NULL;
        if (r->dir && d == NULL) {
            ERR(c, r->pos, "RP1315", "dir '%s' is not a dir ID", r->dir);
            continue;
        }
        if (r->feature && !find_feature(ir, r->feature)) {
            ERR(c, r->pos, "RP1307", "feature '%s' is not defined", r->feature);
        } else if (r->feature == NULL) {
            if (d && d->feature) r->feature = ir_dup(c, d->feature);
            else if (!declared) r->feature = ir_dup(c, "Main");
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
        if (!ir_find_dir(ir, d)) {
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
            const rp_ir_dir_t *d = ir_find_dir(ir, x->target);
            found = d != NULL;
            if (d && d->part_count == 0) ERR(c, x->pos, "RP1316", "a known folder itself ('%s') cannot get permissions", x->target);
            if (d) x->feature = ir_dup(c, d->feature ? d->feature : declared ? NULL : "Main");
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
        const rp_ir_dir_t *d = f && f->dir ? ir_find_dir(ir, f->dir) : NULL;
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
            if (x->name && ir->services[j].name && ir_ascii_casecmp(ir->services[j].name, x->name) == 0) {
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
        if (ir_find_dir(ir, x->property)) {
            if (x->kind != RP_SEARCH_REGISTRY && x->kind != RP_SEARCH_DIR) {
                ERR(c, x->pos, "RP1310", "'%s' is the name of a dir: only a registry or dir search can fill a dir", x->property);
            } else {
                ir->searches[k].fills_dir = true;
            }
        }
    }
    for (size_t k = 0; k < ir->ini_count; ++k) {
        rp_ir_ini_t *x = &ir->inis[k];
        const rp_ir_dir_t *d = x->dir ? ir_find_dir(ir, x->dir) : NULL;
        if (x->dir && d == NULL) {
            ERR(c, x->pos, "RP1315", "dir '%s' is not a dir ID", x->dir);
            continue;
        }
        if (x->feature && !find_feature(ir, x->feature)) {
            ERR(c, x->pos, "RP1307", "feature '%s' is not defined", x->feature);
        } else if (x->feature == NULL) {
            if (d && d->feature) x->feature = ir_dup(c, d->feature);
            else if (!declared) x->feature = ir_dup(c, "Main");
            else ERR(c, x->pos, "RP1202", "[ini.%s] needs a feature: set 'feature' here or on its dir", x->id);
        }
    }
    for (size_t k = 0; k < ir->merge_count; ++k) {
        rp_ir_merge_t *x = &ir->merges[k];
        if (x->dir && ir_find_dir(ir, x->dir) == NULL) ERR(c, x->pos, "RP1315", "dir '%s' is not a dir ID", x->dir);
        const rp_ir_dir_t *d = x->dir ? ir_find_dir(ir, x->dir) : NULL;
        if (x->feature && !find_feature(ir, x->feature)) {
            ERR(c, x->pos, "RP1307", "feature '%s' is not defined", x->feature);
        } else if (x->feature == NULL) {
            if (d && d->feature) x->feature = ir_dup(c, d->feature);
            else if (!declared) x->feature = ir_dup(c, "Main");
            else ERR(c, x->pos, "RP1202", "[merge.%s] needs a feature: set 'feature' here or on its dir", x->id);
        }
    }
    for (size_t k = 0; k < ir->env_count; ++k) {
        rp_ir_env_t *e = &ir->envs[k];
        if (e->feature && !find_feature(ir, e->feature)) {
            ERR(c, e->pos, "RP1307", "feature '%s' is not defined", e->feature);
        } else if (e->feature == NULL) {
            if (!declared) e->feature = ir_dup(c, "Main");
            else ERR(c, e->pos, "RP1202", "[env.%s] needs a feature", e->id);
        }
    }
    for (size_t k = 0; k < ir->copy_count; ++k) {
        const rp_ir_copy_t *cp = &ir->copies[k];
        bool found = false;
        for (size_t j = 0; cp->source_file && j < ir->file_count; ++j) found |= strcmp(ir->files[j].id, cp->source_file) == 0;
        if (cp->source_file && !found) ERR(c, cp->pos, "RP1315", "source: file '%s' is not a [file.*] of this package", cp->source_file);
        if (cp->dir && !ir_find_dir(ir, cp->dir)) ERR(c, cp->pos, "RP1315", "dir '%s' is not a dir ID", cp->dir);
    }
    // Shortcuts: target file and folders exist.
    for (size_t k = 0; k < ir->shortcut_count; ++k) {
        const rp_ir_shortcut_t *sc = &ir->shortcuts[k];
        bool found = false;
        for (size_t j = 0; sc->target_file && j < ir->file_count; ++j) found |= strcmp(ir->files[j].id, sc->target_file) == 0;
        if (sc->target_file && !found) ERR(c, sc->pos, "RP1315", "target: file '%s' is not a [file.*] of this package", sc->target_file);
        if (sc->dir && !ir_shortcut_folder(sc->dir) && !ir_find_dir(ir, sc->dir)) {
            ERR(c, sc->pos, "RP1315", "dir '%s' is neither a dir ID nor Programs, Desktop, StartMenu or Startup", sc->dir);
        }
        if (sc->working_dir && !ir_find_dir(ir, sc->working_dir)) {
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
