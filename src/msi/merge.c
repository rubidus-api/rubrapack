// src/msi/merge.c - merge modules (.msm) merged into a finished table set (RFC-0016 3).
//
// A merge module is an MSI database whose keys carry the module's GUID, with a few Module* tables
// and its files in the stream MergeModule.CABinet. Merging, as Microsoft Learn ("Merge Modules",
// "Merging the module") describes it: every ordinary table's rows join the package's (a table the
// package lacks is added); the module's TARGETDIR row goes, and its children hang under the dir
// the source names; the files' sequence numbers move past the package's own, and the module's
// cabinet goes in unchanged as a cabinet of its own (one more Media row); ModuleComponents become
// FeatureComponents rows of the feature the source names (ModuleSignature and ModuleComponents
// themselves stay, as mergemod.dll leaves them); the Module*Sequence rows place their
// actions by Sequence, or next to BaseAction (After 1: just after, 0: just before).

#include "rubrapack/merge.h"
#include "rubrapack/mem.h"
#include "rubrapack/pal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MAX_MSM = 1u << 30 };

static bool cell_is(const rp_msi_cell_t *c, const char *s) {
    size_t n = strlen(s);
    return c->kind == RP_MSI_STR && c->len == n && (n == 0 || memcmp(c->bytes, s, n) == 0);
}

static rp_msi_cell_t cstr(const char *s) {
    return s ? (rp_msi_cell_t){ .kind = RP_MSI_STR, .bytes = (const uint8_t *)s, .len = strlen(s) } : (rp_msi_cell_t){ .kind = RP_MSI_NULL };
}

static void *keep_alloc(rp_msi_module_t *m, size_t n, size_t size) {
    if (m->nkept == m->capkept) {
        size_t cap = m->capkept ? m->capkept * 2 : 16;
        void **k = rp_mem_alloc(m->alloc, cap, sizeof *k);
        if (k == NULL) return NULL;
        if (m->nkept) memcpy(k, m->kept, m->nkept * sizeof *k);
        rp_mem_free(m->alloc, m->kept);
        m->kept = k;
        m->capkept = cap;
    }
    void *p = rp_mem_alloc(m->alloc, n ? n : 1, size);
    if (p) m->kept[m->nkept++] = p;
    return p;
}

static char *keep_str(rp_msi_module_t *m, const char *s) {
    char *p = keep_alloc(m, strlen(s) + 1, 1);
    if (p) memcpy(p, s, strlen(s) + 1);
    return p;
}

static const rp_msi_wtable_t *mod_table(const rp_msi_module_t *m, const char *name) {
    for (size_t t = 0; t < m->view.db.table_count; ++t) {
        if (strcmp(m->view.db.tables[t].name, name) == 0) return &m->view.db.tables[t];
    }
    return NULL;
}

static int col_of(const rp_msi_wtable_t *t, const char *name) {
    for (size_t c = 0; c < t->column_count; ++c) {
        if (strcmp(t->columns[c].name, name) == 0) return (int)c;
    }
    return -1;
}

proven_err_t rp_msi_module_open(proven_allocator_t alloc, const char *path, rp_msi_module_t *m, const char **why) {
    memset(m, 0, sizeof *m);
    m->alloc = alloc;
    *why = NULL;
    proven_err_t err = rp_pal_read_file(alloc, path, MAX_MSM, &m->data, &m->len);
    if (err != PROVEN_OK) {
        *why = "cannot be read";
        return err;
    }
    rp_limits_t lim = rp_limits_default();
    if (rp_cfb_open(&m->cfb, alloc, m->data, m->len, &lim, why) != PROVEN_OK) {
        if (*why == NULL) *why = "is not a compound file";
        return PROVEN_ERR_INVALID_FORMAT;
    }
    m->cfb_open = true;
    if (rp_msi_open(&m->msi, alloc, &m->cfb, &lim, why) != PROVEN_OK) {
        if (*why == NULL) *why = "is not a Windows Installer database";
        return PROVEN_ERR_INVALID_FORMAT;
    }
    m->msi_open = true;
    if (rp_msi_view(&m->msi, NULL, 0, &m->view) != PROVEN_OK) {
        *why = "has a table that cannot be read";
        return PROVEN_ERR_INVALID_FORMAT;
    }
    m->view_open = true;
    const rp_msi_wtable_t *sig = mod_table(m, "ModuleSignature");
    if (sig == NULL || sig->row_count == 0) {
        *why = "has no ModuleSignature: not a merge module";
        return PROVEN_ERR_INVALID_FORMAT;
    }
    // ModuleID is <name>.<GUID with underscores>
    const rp_msi_cell_t *id = &sig->cells[0];
    size_t n = id->kind == RP_MSI_STR && id->len < sizeof m->module_id ? id->len : 0;
    memcpy(m->module_id, id->bytes, n);
    m->module_id[n] = 0;
    const rp_msi_wtable_t *cfg = mod_table(m, "ModuleConfiguration");
    if (cfg && cfg->row_count) {
        *why = "is a configurable merge module (ModuleConfiguration), which rubrapack does not merge";
        return PROVEN_ERR_UNSUPPORTED;
    }
    // Strings outside ASCII must already be UTF-8: the package's code page is 65001.
    if (m->msi.codepage != 65001) {
        for (size_t i = 0; i < m->msi.string_count; ++i) {
            if (m->msi.strings[i].non_ascii) {
                *why = "has text outside ASCII in another code page than 65001 (UTF-8)";
                return PROVEN_ERR_UNSUPPORTED;
            }
        }
    }
    // The cabinet.
    uint16_t name[32];
    size_t nl;
    uint32_t sid;
    if (rp_msi_stream_name("MergeModule.CABinet", false, name, &nl) == PROVEN_OK && rp_cfb_find(&m->cfb, 0, name, nl, &sid) == PROVEN_OK) {
        m->cab_len = (size_t)m->cfb.entries[sid].size;
        m->cab = rp_mem_alloc(alloc, m->cab_len ? m->cab_len : 1, 1);
        if (m->cab == NULL) return PROVEN_ERR_NOMEM;
        if (rp_cfb_read(&m->cfb, sid, m->cab, m->cab_len) != PROVEN_OK) {
            *why = "has a MergeModule.CABinet stream that cannot be read";
            return PROVEN_ERR_INVALID_FORMAT;
        }
    }
    return PROVEN_OK;
}

void rp_msi_module_close(rp_msi_module_t *m) {
    for (size_t i = 0; i < m->nkept; ++i) rp_mem_free(m->alloc, m->kept[i]);
    rp_mem_free(m->alloc, m->kept);
    rp_mem_free(m->alloc, m->cab);
    if (m->view_open) rp_msi_view_free(&m->msi, &m->view);
    if (m->msi_open) rp_msi_close(&m->msi);
    if (m->cfb_open) rp_cfb_close(&m->cfb);
    rp_mem_free(m->alloc, m->data);
    memset(m, 0, sizeof *m);
}

const rp_msi_wtable_t *rp_msi_module_validation(const rp_msi_module_t *m) { return mod_table(m, "_Validation"); }

// A binary cell's bytes: the module stream "<Table>.<key>".
static proven_err_t binary_bytes(rp_msi_module_t *m, const rp_msi_wtable_t *t, const rp_msi_cell_t *row, rp_msi_cell_t *cell) {
    char key[300];
    if (row[0].kind != RP_MSI_STR || row[0].len + strlen(t->name) + 2 > sizeof key) return PROVEN_ERR_INVALID_FORMAT;
    snprintf(key, sizeof key, "%s.%.*s", t->name, (int)row[0].len, (const char *)row[0].bytes);
    uint16_t name[32];
    size_t nl;
    uint32_t sid;
    if (rp_msi_stream_name(key, false, name, &nl) != PROVEN_OK || rp_cfb_find(&m->cfb, 0, name, nl, &sid) != PROVEN_OK) {
        *cell = (rp_msi_cell_t){ .kind = RP_MSI_NULL };
        return PROVEN_OK;
    }
    size_t n = (size_t)m->cfb.entries[sid].size;
    uint8_t *b = keep_alloc(m, n, 1);
    if (b == NULL) return PROVEN_ERR_NOMEM;
    if (rp_cfb_read(&m->cfb, sid, b, n) != PROVEN_OK) return PROVEN_ERR_INVALID_FORMAT;
    *cell = (rp_msi_cell_t){ .kind = RP_MSI_BINARY, .bytes = b, .len = n };
    return PROVEN_OK;
}

static rp_msi_wtable_t *pkg_table(rp_msi_wtable_t *tables, size_t nt, const char *name) {
    for (size_t t = 0; t < nt; ++t) {
        if (strcmp(tables[t].name, name) == 0) return &tables[t];
    }
    return NULL;
}

// Rows added to a package table: a new cell array (old rows, then these), kept by the module.
static proven_err_t append_rows(rp_msi_module_t *m, rp_msi_wtable_t *t, const rp_msi_cell_t *rows, size_t n) {
    if (n == 0) return PROVEN_OK;
    size_t cc = t->column_count;
    rp_msi_cell_t *cells = keep_alloc(m, (t->row_count + n) * cc, sizeof *cells);
    if (cells == NULL) return PROVEN_ERR_NOMEM;
    if (t->row_count) memcpy(cells, t->cells, t->row_count * cc * sizeof *cells);
    memcpy(cells + t->row_count * cc, rows, n * cc * sizeof *cells);
    t->cells = cells;
    t->row_count += n;
    return PROVEN_OK;
}

static int32_t max_int(const rp_msi_wtable_t *t, const char *col) {
    int c = t ? col_of(t, col) : -1;
    int32_t best = 0;
    for (size_t r = 0; c >= 0 && r < t->row_count; ++r) {
        const rp_msi_cell_t *x = &t->cells[r * t->column_count + (size_t)c];
        if (x->kind == RP_MSI_INT && x->i > best) best = x->i;
    }
    return best;
}

static bool seq_has(const rp_msi_wtable_t *t, const char *action, int32_t *seq) {
    for (size_t r = 0; t && r < t->row_count; ++r) {
        const rp_msi_cell_t *row = &t->cells[r * t->column_count];
        if (cell_is(&row[0], action)) {
            if (seq) *seq = row[2].kind == RP_MSI_INT ? row[2].i : 0;
            return true;
        }
    }
    return false;
}

static bool seq_taken(const rp_msi_wtable_t *t, int32_t n) {
    for (size_t r = 0; t && r < t->row_count; ++r) {
        const rp_msi_cell_t *x = &t->cells[r * t->column_count + 2];
        if (x->kind == RP_MSI_INT && x->i == n) return true;
    }
    return false;
}

// The Module*Sequence rows of one sequence table: placed by Sequence, or next to BaseAction.
static proven_err_t merge_sequence(rp_msi_module_t *m, rp_msi_wtable_t *tables, size_t nt, const char *mod_name,
                                   const char *pkg_name, const char **why) {
    const rp_msi_wtable_t *ms = mod_table(m, mod_name);
    rp_msi_wtable_t *ps = pkg_table(tables, nt, pkg_name);
    if (ms == NULL || ms->row_count == 0) return PROVEN_OK;
    if (ps == NULL) {
        *why = "has actions for a sequence table the package does not have";
        return PROVEN_ERR_UNSUPPORTED;
    }
    int ca = col_of(ms, "Action"), cs = col_of(ms, "Sequence"), cb = col_of(ms, "BaseAction"), cf = col_of(ms, "After"),
        cc = col_of(ms, "Condition");
    if (ca < 0 || cs < 0 || cb < 0 || cf < 0 || cc < 0) {
        *why = "has a Module*Sequence table without the standard columns";
        return PROVEN_ERR_INVALID_FORMAT;
    }
    bool *done = keep_alloc(m, ms->row_count, sizeof *done);
    if (done == NULL) return PROVEN_ERR_NOMEM;
    memset(done, 0, ms->row_count * sizeof *done);
    // Rows may lean on each other (BaseAction another module action): place until nothing moves.
    for (size_t pass = 0; pass <= ms->row_count; ++pass) {
        bool moved = false;
        for (size_t r = 0; r < ms->row_count; ++r) {
            if (done[r]) continue;
            const rp_msi_cell_t *row = &ms->cells[r * ms->column_count];
            char action[128];
            if (row[ca].kind != RP_MSI_STR || row[ca].len >= sizeof action) continue;
            memcpy(action, row[ca].bytes, row[ca].len);
            action[row[ca].len] = 0;
            if (seq_has(ps, action, NULL)) {        // a standard action the package already runs
                done[r] = moved = true;
                continue;
            }
            int32_t seq;
            if (row[cs].kind == RP_MSI_INT) {
                seq = row[cs].i;
            } else {
                char base[128];
                if (row[cb].kind != RP_MSI_STR || row[cb].len >= sizeof base) continue;
                memcpy(base, row[cb].bytes, row[cb].len);
                base[row[cb].len] = 0;
                int32_t b;
                if (!seq_has(ps, base, &b)) continue;   // not placed yet: a later pass
                bool after = row[cf].kind == RP_MSI_INT && row[cf].i != 0;
                seq = after ? b + 1 : b - 1;
                while (seq_taken(ps, seq) && seq > 0 && seq < 32767) seq += after ? 1 : -1;
            }
            rp_msi_cell_t add[3] = { cstr(keep_str(m, action)), row[cc], { .kind = RP_MSI_INT, .i = seq } };
            if (add[0].bytes == NULL) return PROVEN_ERR_NOMEM;
            proven_err_t err = append_rows(m, ps, add, 1);
            if (err != PROVEN_OK) return err;
            done[r] = moved = true;
        }
        if (!moved) break;
    }
    for (size_t r = 0; r < ms->row_count; ++r) {
        if (!done[r]) {
            *why = "sequences an action next to one that neither the package nor the module runs";
            return PROVEN_ERR_INVALID_FORMAT;
        }
    }
    return PROVEN_OK;
}

proven_err_t rp_msi_module_merge(rp_msi_module_t *m, rp_msi_wtable_t *tables, size_t *nt, size_t cap, const char *dir_key,
                                 const char *feature, unsigned index, rp_msi_wstream_t *cab_stream, const char **why) {
    *why = NULL;
    rp_msi_wtable_t *file = pkg_table(tables, *nt, "File");
    int32_t base = max_int(file, "Sequence");
    int32_t last = base;
    // Ordinary tables.
    for (size_t t = 0; t < m->view.db.table_count; ++t) {
        const rp_msi_wtable_t *mt = &m->view.db.tables[t];
        // ModuleSignature and ModuleComponents stay in the package, as Microsoft's mergemod.dll
        // leaves them (they name the merged modules); the Module*Sequence tables are placed below.
        size_t nl = strlen(mt->name);
        bool module_seq = strncmp(mt->name, "Module", 6) == 0 && nl > 8 && strcmp(mt->name + nl - 8, "Sequence") == 0;
        if (module_seq || strcmp(mt->name, "_Validation") == 0 || mt->row_count == 0) continue;
        size_t cc = mt->column_count;
        rp_msi_cell_t *rows = keep_alloc(m, mt->row_count * cc, sizeof *rows);
        if (rows == NULL) return PROVEN_ERR_NOMEM;
        size_t n = 0;
        bool is_dir = strcmp(mt->name, "Directory") == 0, is_file = strcmp(mt->name, "File") == 0,
             is_prop = strcmp(mt->name, "Property") == 0;
        int seq_col = is_file ? col_of(mt, "Sequence") : -1;
        rp_msi_wtable_t *pt = pkg_table(tables, *nt, mt->name);
        for (size_t r = 0; r < mt->row_count; ++r) {
            const rp_msi_cell_t *src = &mt->cells[r * cc];
            if (is_dir && cell_is(&src[0], "TARGETDIR")) continue;
            if (is_prop && pt) {                    // the package's own value wins
                bool have = false;
                for (size_t q = 0; q < pt->row_count && !have; ++q) {
                    const rp_msi_cell_t *k = &pt->cells[q * pt->column_count];
                    have = k->kind == RP_MSI_STR && src[0].kind == RP_MSI_STR && k->len == src[0].len &&
                           memcmp(k->bytes, src[0].bytes, k->len) == 0;
                }
                if (have) continue;
            }
            rp_msi_cell_t *dst = &rows[n++ * cc];
            memcpy(dst, src, cc * sizeof *dst);
            for (size_t c = 0; c < cc; ++c) {
                if ((mt->columns[c].type & 0x0800) && !(mt->columns[c].type & 0x0400)) {   // OBJECT: the stream
                    proven_err_t err = binary_bytes(m, mt, src, &dst[c]);
                    if (err != PROVEN_OK) return err;
                }
            }
            if (is_dir && cell_is(&dst[1], "TARGETDIR")) dst[1] = cstr(dir_key);
            if (seq_col >= 0 && dst[seq_col].kind == RP_MSI_INT) {
                dst[seq_col].i += base;
                if (dst[seq_col].i > last) last = dst[seq_col].i;
            }
        }
        if (pt) {
            if (pt->column_count != cc) {
                *why = "has a table whose columns differ from the package's";
                return PROVEN_ERR_UNSUPPORTED;
            }
            for (size_t c = 0; c < cc; ++c) {
                if (strcmp(pt->columns[c].name, mt->columns[c].name) != 0) {
                    *why = "has a table whose columns differ from the package's";
                    return PROVEN_ERR_UNSUPPORTED;
                }
            }
            proven_err_t err = append_rows(m, pt, rows, n);
            if (err != PROVEN_OK) return err;
        } else {
            if (*nt == cap) {
                *why = "adds too many tables";
                return PROVEN_ERR_OUT_OF_BOUNDS;
            }
            tables[(*nt)++] = (rp_msi_wtable_t){ mt->name, mt->columns, cc, rows, n };
        }
    }
    // Components into the feature.
    const rp_msi_wtable_t *mc = mod_table(m, "ModuleComponents");
    rp_msi_wtable_t *fc = pkg_table(tables, *nt, "FeatureComponents");
    if (mc && mc->row_count) {
        if (fc == NULL) {
            *why = "has components, but the package has no FeatureComponents table";
            return PROVEN_ERR_INVALID_STATE;
        }
        rp_msi_cell_t *rows = keep_alloc(m, mc->row_count * 2, sizeof *rows);
        if (rows == NULL) return PROVEN_ERR_NOMEM;
        int c = col_of(mc, "Component");
        for (size_t r = 0; r < mc->row_count; ++r) {
            rows[2 * r] = cstr(feature);
            rows[2 * r + 1] = mc->cells[r * mc->column_count + (size_t)(c < 0 ? 0 : c)];
        }
        proven_err_t err = append_rows(m, fc, rows, mc->row_count);
        if (err != PROVEN_OK) return err;
    }
    // Actions.
    static const char *const seqs[][2] = { { "ModuleInstallExecuteSequence", "InstallExecuteSequence" },
                                           { "ModuleInstallUISequence", "InstallUISequence" },
                                           { "ModuleAdminExecuteSequence", "AdminExecuteSequence" },
                                           { "ModuleAdminUISequence", "AdminUISequence" },
                                           { "ModuleAdvtExecuteSequence", "AdvtExecuteSequence" } };
    for (size_t s = 0; s < sizeof seqs / sizeof seqs[0]; ++s) {
        proven_err_t err = merge_sequence(m, tables, *nt, seqs[s][0], seqs[s][1], why);
        if (err != PROVEN_OK) return err;
    }
    // A module that uses a table without listing its standard actions still gets them, at the
    // numbers the package itself uses for them (src/msi/lower.c).
    static const struct { const char *table, *action; int seq; } std[] = {
        { "Registry", "RemoveRegistryValues", 2600 }, { "Registry", "WriteRegistryValues", 5000 },
        { "RemoveRegistry", "RemoveRegistryValues", 2600 }, { "RemoveRegistry", "WriteRegistryValues", 5000 },
        { "Shortcut", "RemoveShortcuts", 3200 }, { "Shortcut", "CreateShortcuts", 4500 },
        { "Environment", "RemoveEnvironmentStrings", 3310 }, { "Environment", "WriteEnvironmentStrings", 5200 },
        { "IniFile", "RemoveIniValues", 3320 }, { "IniFile", "WriteIniValues", 5100 },
        { "RemoveIniFile", "RemoveIniValues", 3320 }, { "DuplicateFile", "RemoveDuplicateFiles", 3300 },
        { "DuplicateFile", "DuplicateFiles", 4210 }, { "CreateFolder", "RemoveFolders", 3600 },
        { "CreateFolder", "CreateFolders", 3700 }, { "Font", "UnregisterFonts", 2500 }, { "Font", "RegisterFonts", 5300 },
        { "ServiceInstall", "DeleteServices", 2000 }, { "ServiceInstall", "InstallServices", 5800 },
        { "ServiceControl", "StopServices", 1900 }, { "ServiceControl", "StartServices", 5900 },
        { "SelfReg", "SelfUnregModules", 2200 }, { "SelfReg", "SelfRegModules", 6500 },
    };
    rp_msi_wtable_t *ie = pkg_table(tables, *nt, "InstallExecuteSequence");
    for (size_t k = 0; ie && k < sizeof std / sizeof std[0]; ++k) {
        const rp_msi_wtable_t *t = pkg_table(tables, *nt, std[k].table);
        if (t == NULL || t->row_count == 0 || seq_has(ie, std[k].action, NULL)) continue;
        rp_msi_cell_t add[3] = { cstr(std[k].action), { .kind = RP_MSI_NULL }, { .kind = RP_MSI_INT, .i = std[k].seq } };
        proven_err_t err = append_rows(m, ie, add, 1);
        if (err != PROVEN_OK) return err;
    }
    // The cabinet, as a Media row of its own.
    *cab_stream = (rp_msi_wstream_t){ 0 };
    if (last > base) {
        if (m->cab == NULL) {
            *why = "has files but no MergeModule.CABinet stream";
            return PROVEN_ERR_INVALID_FORMAT;
        }
        rp_msi_wtable_t *media = pkg_table(tables, *nt, "Media");
        if (media == NULL || media->column_count != 6) {
            *why = "has files, but the package has no Media table";
            return PROVEN_ERR_INVALID_STATE;
        }
        char name[32], ref[34];
        snprintf(name, sizeof name, "msm%u.cab", index);
        snprintf(ref, sizeof ref, "#%s", name);
        char *sn = keep_str(m, name), *sr = keep_str(m, ref);
        if (sn == NULL || sr == NULL) return PROVEN_ERR_NOMEM;
        rp_msi_cell_t row[6] = { { .kind = RP_MSI_INT, .i = max_int(media, "DiskId") + 1 }, { .kind = RP_MSI_INT, .i = last },
                                 { .kind = RP_MSI_NULL }, cstr(sr), { .kind = RP_MSI_NULL }, { .kind = RP_MSI_NULL } };
        proven_err_t err = append_rows(m, media, row, 1);
        if (err != PROVEN_OK) return err;
        *cab_stream = (rp_msi_wstream_t){ sn, m->cab, m->cab_len };
    }
    return PROVEN_OK;
}
