// src/msix/msix_merge.c - merge modules in an MSIX (RFC-0019): what an MSIX can carry of a module.
//
// A module merged into an MSIX gives its files and its registry values; nothing runs at install.
// Its files go where its Directory table puts them: below its TARGETDIR (where [merge.ID] dir
// says), or below a standard folder the package's virtual file system has. Its registry values go
// into the package's hives. A module that needs more - custom actions, conditions on components,
// environment or INI entries, services, fonts - is refused, as is a value filled in at install time.

#include "rubrapack/cab.h"
#include "rubrapack/mem.h"
#include "rubrapack/merge.h"
#include "rubrapack/msix.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const rp_msi_wtable_t *table(const rp_msi_module_t *m, const char *name) {
    for (size_t t = 0; t < m->ntabs; ++t) {
        if (strcmp(m->tabs[t].name, name) == 0) return &m->tabs[t];
    }
    return NULL;
}

static int col(const rp_msi_wtable_t *t, const char *name) {
    for (size_t c = 0; t && c < t->column_count; ++c) {
        if (strcmp(t->columns[c].name, name) == 0) return (int)c;
    }
    return -1;
}

static bool is(const rp_msi_cell_t *c, const char *s) {
    size_t n = strlen(s);
    return c->kind == RP_MSI_STR && c->len == n && memcmp(c->bytes, s, n) == 0;
}

static char *text(proven_allocator_t a, const rp_msi_cell_t *c) {
    size_t n = c->kind == RP_MSI_STR ? c->len : 0;
    char *s = rp_mem_alloc(a, n + 1, 1);
    if (s == NULL) return NULL;
    if (n) memcpy(s, c->bytes, n);
    s[n] = '\0';
    return s;
}

// A DefaultDir's name on the target: "target:source" -> target; "short|long" -> long; "." -> none.
static void dir_name(const char *dd, char *out, size_t cap) {
    char t[512];
    snprintf(t, sizeof t, "%s", dd);
    char *colon = strchr(t, ':');
    if (colon) *colon = '\0';
    char *bar = strchr(t, '|');
    snprintf(out, cap, "%s", bar ? bar + 1 : t);
    if (strcmp(out, ".") == 0) out[0] = '\0';
}

// The VFS folder of a standard folder property, or NULL.
static const char *vfs_of(const char *prop) {
    static const char *const map[][2] = { { "ProgramFiles64Folder", "ProgramFilesX64" }, { "ProgramFilesFolder", "ProgramFilesX86" },
                                          { "CommonFiles64Folder", "ProgramFilesCommonX64" }, { "CommonFilesFolder", "ProgramFilesCommonX86" },
                                          { "System64Folder", "SystemX64" }, { "SystemFolder", "SystemX86" }, { "WindowsFolder", "Windows" },
                                          { "CommonAppDataFolder", "Common AppData" } };
    for (size_t k = 0; k < sizeof map / sizeof map[0]; ++k) {
        if (strcmp(prop, map[k][0]) == 0) return map[k][1];
    }
    return NULL;
}

// The path of a module directory: "sub\\dir" below the module's root (*vfs NULL), or below a
// standard folder (*vfs its VFS folder). False when it leads elsewhere.
static bool dir_path(proven_allocator_t a, const rp_msi_wtable_t *dirs, const rp_msi_cell_t *key, const char **vfs, char *out, size_t cap,
                     const char **why) {
    int ck = col(dirs, "Directory"), cp = col(dirs, "Directory_Parent"), cd = col(dirs, "DefaultDir");
    char parts[32][512];
    size_t n = 0;
    const rp_msi_cell_t *k = key;
    *vfs = NULL;
    for (int depth = 0; depth < 32; ++depth) {
        if (is(k, "TARGETDIR")) break;
        const rp_msi_cell_t *row = NULL;
        for (size_t r = 0; r < dirs->row_count && !row; ++r) {
            const rp_msi_cell_t *c = &dirs->cells[r * dirs->column_count];
            if (c[ck].kind == RP_MSI_STR && k->kind == RP_MSI_STR && c[ck].len == k->len && memcmp(c[ck].bytes, k->bytes, k->len) == 0) row = c;
        }
        if (row == NULL) {
            *why = "names a folder its Directory table does not have";
            return false;
        }
        char *name = text(a, &row[ck]);
        if (name == NULL) return false;
        // A standard folder (its key, before the module's GUID, is the property's name).
        char base[128];
        snprintf(base, sizeof base, "%s", name);
        char *dot = strchr(base, '.');
        if (dot) *dot = '\0';
        const char *v = vfs_of(base);
        rp_mem_free(a, name);
        if (v && (row[cp].kind != RP_MSI_STR || is(&row[cp], "TARGETDIR"))) {
            *vfs = v;
            break;
        }
        char *dd = text(a, &row[cd]);
        if (dd == NULL) return false;
        dir_name(dd, parts[n], sizeof parts[n]);
        rp_mem_free(a, dd);
        if (parts[n][0]) ++n;
        if (row[cp].kind != RP_MSI_STR) {
            *why = "has a folder outside its TARGETDIR that an MSIX has no place for";
            return false;
        }
        k = &row[cp];
    }
    size_t o = 0;
    out[0] = '\0';
    for (size_t i = n; i-- > 0;) {
        int w = snprintf(out + o, cap - o, "%s%s", o ? "\\" : "", parts[i]);
        if (w < 0 || (size_t)w >= cap - o) return false;
        o += (size_t)w;
    }
    return true;
}

void rp_msix_module_free(proven_allocator_t a, rp_msix_module_t *x) {
    for (size_t i = 0; i < x->file_count; ++i) {
        rp_mem_free(a, x->files[i].rel);
        rp_mem_free(a, x->files[i].data);
    }
    rp_mem_free(a, x->files);
    for (size_t i = 0; i < x->reg_count; ++i) {
        rp_mem_free(a, x->regs[i].key);
        rp_mem_free(a, x->regs[i].name);
        rp_mem_free(a, x->regs[i].value);
        for (size_t k = 0; k < x->regs[i].item_count; ++k) rp_mem_free(a, x->regs[i].items[k]);
        rp_mem_free(a, x->regs[i].items);
    }
    rp_mem_free(a, x->regs);
    memset(x, 0, sizeof *x);
}

proven_err_t rp_msix_module_read(proven_allocator_t a, const char *path, const char *const *config, size_t nconfig, rp_msix_module_t *out,
                                 const char **why) {
    memset(out, 0, sizeof *out);
    *why = NULL;
    rp_msi_module_t m;
    proven_err_t err = rp_msi_module_open(a, path, &m, why);
    if (err == PROVEN_OK) err = rp_msi_module_configure(&m, config, nconfig, "", why);
    // What an MSIX cannot do at install time.
    static const char *const refused[] = { "CustomAction", "Environment", "IniFile", "RemoveIniFile", "ServiceInstall", "ServiceControl",
                                           "Font", "SelfReg", "MoveFile", "DuplicateFile", "RemoveFile", "LockPermissions",
                                           "MsiLockPermissionsEx", "Class", "ProgId", "Extension", "Verb", "TypeLib", "AppId", NULL };
    for (size_t k = 0; err == PROVEN_OK && refused[k]; ++k) {
        const rp_msi_wtable_t *t = table(&m, refused[k]);
        if (t && t->row_count) {
            static char msg[128];
            snprintf(msg, sizeof msg, "has %s rows, which an MSIX cannot carry", refused[k]);
            *why = msg;
            err = PROVEN_ERR_UNSUPPORTED;
        }
    }
    const rp_msi_wtable_t *comps = table(&m, "Component"), *files = table(&m, "File"), *dirs = table(&m, "Directory"),
                          *reg = table(&m, "Registry");
    if (err == PROVEN_OK && comps) {
        int cc = col(comps, "Condition");
        for (size_t r = 0; cc >= 0 && r < comps->row_count; ++r) {
            const rp_msi_cell_t *c = &comps->cells[r * comps->column_count + (size_t)cc];
            if (c->kind == RP_MSI_STR && c->len) {
                *why = "has a component with a condition; an MSIX installs everything";
                err = PROVEN_ERR_UNSUPPORTED;
            }
        }
    }
    // Files: their bytes from MergeModule.CABinet, their places from Directory.
    rp_cab_file_t *ents = NULL;
    size_t nents = 0;
    uint8_t *arena = NULL;
    if (err == PROVEN_OK && files && files->row_count) {
        if (m.cab == NULL || dirs == NULL || comps == NULL) {
            *why = "has files but no MergeModule.CABinet or Directory table";
            err = PROVEN_ERR_INVALID_FORMAT;
        } else {
            rp_limits_t lim = rp_limits_default();
            err = rp_cab_read(a, m.cab, m.cab_len, &lim, &ents, &nents, &arena);
            if (err != PROVEN_OK) *why = "has a MergeModule.CABinet that cannot be read";
        }
    }
    if (err == PROVEN_OK && files && files->row_count) {
        out->files = rp_mem_alloc(a, files->row_count, sizeof *out->files);
        if (out->files == NULL) err = PROVEN_ERR_NOMEM;
        int fk = col(files, "File"), fc = col(files, "Component_"), fn = col(files, "FileName");
        int ck = col(comps, "Component"), cdir = col(comps, "Directory_");
        for (size_t r = 0; err == PROVEN_OK && r < files->row_count; ++r) {
            const rp_msi_cell_t *f = &files->cells[r * files->column_count];
            const rp_msi_cell_t *crow = NULL;
            for (size_t q = 0; q < comps->row_count && !crow; ++q) {
                const rp_msi_cell_t *c = &comps->cells[q * comps->column_count];
                if (c[ck].kind == RP_MSI_STR && f[fc].kind == RP_MSI_STR && c[ck].len == f[fc].len && memcmp(c[ck].bytes, f[fc].bytes, f[fc].len) == 0) crow = c;
            }
            const rp_cab_file_t *e = NULL;
            for (size_t q = 0; q < nents && !e; ++q) {
                if (f[fk].kind == RP_MSI_STR && strlen(ents[q].name) == f[fk].len && memcmp(ents[q].name, f[fk].bytes, f[fk].len) == 0) e = &ents[q];
            }
            if (crow == NULL || e == NULL) {
                *why = "has a file whose component or cabinet entry is missing";
                err = PROVEN_ERR_INVALID_FORMAT;
                break;
            }
            char dpath[2048], fname[512];
            const char *vfs = NULL;
            if (!dir_path(a, dirs, &crow[cdir], &vfs, dpath, sizeof dpath, why)) {
                err = PROVEN_ERR_UNSUPPORTED;
                break;
            }
            char *fnv = text(a, &f[fn]);
            if (fnv == NULL) {
                err = PROVEN_ERR_NOMEM;
                break;
            }
            char *bar = strchr(fnv, '|');
            snprintf(fname, sizeof fname, "%s", bar ? bar + 1 : fnv);
            rp_mem_free(a, fnv);
            rp_msix_module_file_t *mf = &out->files[out->file_count++];
            size_t rl = strlen(dpath) + strlen(fname) + 2;
            mf->rel = rp_mem_alloc(a, rl, 1);
            mf->data = rp_mem_alloc(a, e->size ? e->size : 1, 1);
            if (mf->rel == NULL || mf->data == NULL) {
                err = PROVEN_ERR_NOMEM;
                break;
            }
            snprintf(mf->rel, rl, "%s%s%s", dpath, dpath[0] ? "\\" : "", fname);
            memcpy(mf->data, e->data, e->size);
            mf->len = e->size;
            mf->vfs = vfs;
        }
    }
    rp_mem_free(a, ents);
    rp_mem_free(a, arena);
    // Registry values, as [registry.*] tables would give them.
    if (err == PROVEN_OK && reg && reg->row_count) {
        out->regs = rp_mem_alloc(a, reg->row_count, sizeof *out->regs);
        if (out->regs == NULL) err = PROVEN_ERR_NOMEM;
        int rr = col(reg, "Root"), rk = col(reg, "Key"), rn = col(reg, "Name"), rv = col(reg, "Value");
        for (size_t r = 0; err == PROVEN_OK && r < reg->row_count; ++r) {
            const rp_msi_cell_t *c = &reg->cells[r * reg->column_count];
            char *name = c[rn].kind == RP_MSI_STR && c[rn].len ? text(a, &c[rn]) : NULL;
            if (name && (strcmp(name, "+") == 0 || strcmp(name, "*") == 0 || strcmp(name, "-") == 0) && c[rv].kind != RP_MSI_STR) {
                rp_mem_free(a, name);           // key creation and removal markers: nothing to carry
                continue;
            }
            rp_ir_registry_t *x = &out->regs[out->reg_count++];
            memset(x, 0, sizeof *x);
            x->root = c[rr].kind == RP_MSI_INT ? (rp_reg_root_t)c[rr].i : RP_ROOT_HKLM;
            x->key = text(a, &c[rk]);
            x->name = name;
            char *v = c[rv].kind == RP_MSI_STR ? text(a, &c[rv]) : NULL;
            x->type = RP_REG_STRING;
            if (v && v[0] == '#' && v[1] == 'x') {
                x->type = RP_REG_BINARY;
                memmove(v, v + 2, strlen(v + 2) + 1);
            } else if (v && v[0] == '#' && v[1] == '%') {
                x->type = RP_REG_EXPAND;
                memmove(v, v + 2, strlen(v + 2) + 1);
            } else if (v && v[0] == '#' && v[1] == '#') {
                memmove(v, v + 1, strlen(v + 1) + 1);    // "##" is a literal '#'
            } else if (v && v[0] == '#') {
                x->type = RP_REG_DWORD;
                memmove(v, v + 1, strlen(v + 1) + 1);
            } else if (v && strstr(v, "[~]")) {
                x->type = RP_REG_MULTI;
                size_t cnt = 1;
                for (const char *p = v; (p = strstr(p, "[~]")) != NULL; p += 3) ++cnt;
                x->items = rp_mem_alloc(a, cnt, sizeof *x->items);
                if (x->items == NULL) {
                    err = PROVEN_ERR_NOMEM;
                    rp_mem_free(a, v);
                    break;
                }
                char *p = v;
                for (;;) {
                    char *sep = strstr(p, "[~]");
                    size_t len = sep ? (size_t)(sep - p) : strlen(p);
                    if (len) {
                        char *it = rp_mem_alloc(a, len + 1, 1);
                        if (it) {
                            memcpy(it, p, len);
                            it[len] = '\0';
                            x->items[x->item_count++] = it;
                        }
                    }
                    if (!sep) break;
                    p = sep + 3;
                }
                rp_mem_free(a, v);
                v = NULL;
            }
            x->value = v;
            if (x->key == NULL) err = PROVEN_ERR_NOMEM;
        }
    }
    rp_msi_module_close(&m);
    if (err != PROVEN_OK) rp_msix_module_free(a, out);
    return err;
}
