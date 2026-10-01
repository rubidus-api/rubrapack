// src/msi/module_out.c - a merge module (.msm) from the tables of a [module] source (RFC-0017).
//
// The source is lowered as a package would be; this turns those tables into a module, the way
// Microsoft Learn describes one ("Merge Modules", "Merge Module Database"): every key the module
// defines ends in ".<GUID>" (the module's GUID, '-' as '_'), so that it cannot meet a key of the
// package or of another module - and so do the references to them, also the [key] parts of
// formatted values; TARGETDIR and the standard folders keep their names, as the package that
// merges the module redirects TARGETDIR and has the standard folders itself. ModuleSignature
// names the module, ModuleComponents lists its components (the package puts them in a feature),
// ModuleInstallExecuteSequence the standard actions its tables need (FeatureComponents and
// InstallExecuteSequence stay, empty, as ICEM04 asks), and the files go in one
// cabinet, MergeModule.CABinet, under their File keys. What only a package has - features, the
// Property table, sequences, media, upgrades, the package's own custom actions (a [module] source
// cannot ask for any: RFC-0017) - is left out.

#include "rubrapack/cab.h"
#include "rubrapack/merge.h"
#include "rubrapack/mem.h"
#include "rubrapack/suminfo.h"
#include "rubrapack/version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    proven_allocator_t alloc;
    char               sfx[40];     // ".<GUID with underscores>"
    char             **keys;        // the keys the module defines, sorted
    size_t             nkeys, capkeys;
    void             **kept;
    size_t             nkept, capkept;
    bool               nomem;
} mod_t;

static void *keep(mod_t *m, void *p) {
    if (p == NULL) {
        m->nomem = true;
        return NULL;
    }
    if (m->nkept == m->capkept) {
        size_t cap = m->capkept ? m->capkept * 2 : 64;
        void **v = rp_mem_alloc(m->alloc, cap, sizeof *v);
        if (v == NULL) {
            rp_mem_free(m->alloc, p);
            m->nomem = true;
            return NULL;
        }
        if (m->nkept) memcpy(v, m->kept, m->nkept * sizeof *v);
        rp_mem_free(m->alloc, m->kept);
        m->kept = v;
        m->capkept = cap;
    }
    m->kept[m->nkept++] = p;
    return p;
}

static char *cell_str(mod_t *m, const rp_msi_cell_t *c) {
    if (c->kind != RP_MSI_STR) return NULL;
    char *s = keep(m, rp_mem_alloc(m->alloc, c->len + 1, 1));
    if (s == NULL) return NULL;
    memcpy(s, c->bytes, c->len);
    s[c->len] = '\0';
    return s;
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static void add_key(mod_t *m, const rp_msi_cell_t *c) {
    char *s = cell_str(m, c);
    if (s == NULL) return;
    if (m->nkeys == m->capkeys) {
        size_t cap = m->capkeys ? m->capkeys * 2 : 64;
        char **v = rp_mem_alloc(m->alloc, cap, sizeof *v);
        if (v == NULL) {
            m->nomem = true;
            return;
        }
        if (m->nkeys) memcpy(v, m->keys, m->nkeys * sizeof *v);
        rp_mem_free(m->alloc, m->keys);
        m->keys = v;
        m->capkeys = cap;
    }
    m->keys[m->nkeys++] = s;
}

static bool is_key(const mod_t *m, const char *s, size_t n) {
    char buf[256];
    if (n >= sizeof buf) return false;
    memcpy(buf, s, n);
    buf[n] = '\0';
    const char *p = buf;
    return m->nkeys && rp_bsearch(&p, m->keys, m->nkeys, sizeof *m->keys, cmp_str) != NULL;
}

static rp_msi_cell_t str_cell(const char *s) {
    return s ? (rp_msi_cell_t){ .kind = RP_MSI_STR, .bytes = (const uint8_t *)s, .len = strlen(s) } : (rp_msi_cell_t){ .kind = RP_MSI_NULL };
}

// A key cell: with the suffix when the module defines that key.
static rp_msi_cell_t renamed(mod_t *m, const rp_msi_cell_t *c) {
    if (c->kind != RP_MSI_STR || !is_key(m, (const char *)c->bytes, c->len)) return *c;
    char *s = keep(m, rp_mem_alloc(m->alloc, c->len + strlen(m->sfx) + 1, 1));
    if (s == NULL) return *c;
    memcpy(s, c->bytes, c->len);
    strcpy(s + c->len, m->sfx);
    return str_cell(s);
}

// A formatted value: [key], [#key], [!key], [$key] of the module's keys get the suffix.
static rp_msi_cell_t formatted(mod_t *m, const rp_msi_cell_t *c) {
    if (c->kind != RP_MSI_STR) return *c;
    const char *s = (const char *)c->bytes;
    size_t n = c->len, extra = 0;
    for (size_t i = 0; i < n; ++i) extra += s[i] == '[';
    char *o = keep(m, rp_mem_alloc(m->alloc, n + extra * strlen(m->sfx) + 1, 1));
    if (o == NULL) return *c;
    size_t w = 0;
    for (size_t i = 0; i < n;) {
        if (s[i] == '[') {
            const char *close = memchr(s + i + 1, ']', n - i - 1);
            size_t inner = i + 1;
            if (close && inner < n && (s[inner] == '#' || s[inner] == '!' || s[inner] == '$')) ++inner;
            if (close && is_key(m, s + inner, (size_t)(close - (s + inner)))) {
                size_t len = (size_t)(close - (s + i));
                memcpy(o + w, s + i, len);
                w += len;
                strcpy(o + w, m->sfx);
                w += strlen(m->sfx);
                o[w++] = ']';
                i += len + 1;
                continue;
            }
        }
        o[w++] = s[i++];
    }
    o[w] = '\0';
    return str_cell(o);
}

static int column_of(const rp_msi_wtable_t *t, const char *name) {
    for (size_t c = 0; c < t->column_count; ++c) {
        if (strcmp(t->columns[c].name, name) == 0) return (int)c;
    }
    return -1;
}

// The tables a module keeps: their key columns (renamed) and formatted columns.
typedef struct {
    const char *table, *own, *refs[4], *fmt[3];
} keep_rule_t;

static const keep_rule_t kept_tables[] = {
    { "Directory", "Directory", { "Directory_Parent" }, { 0 } },
    { "Component", "Component", { "Directory_", "KeyPath" }, { 0 } },
    { "File", "File", { "Component_" }, { 0 } },
    { "MsiFileHash", NULL, { "File_" }, { 0 } },
    { "CreateFolder", NULL, { "Directory_", "Component_" }, { 0 } },
    { "Registry", "Registry", { "Component_" }, { "Key", "Name", "Value" } },
    { "RemoveRegistry", "RemoveRegistry", { "Component_" }, { "Key", "Name" } },
    { "Environment", "Environment", { "Component_" }, { "Value" } },
    { "IniFile", "IniFile", { "DirProperty", "Component_" }, { "Value" } },
    { "RemoveIniFile", "RemoveIniFile", { "DirProperty", "Component_" }, { "Value" } },
    { "RemoveFile", "FileKey", { "Component_", "DirProperty" }, { 0 } },
    { "DuplicateFile", "FileKey", { "Component_", "File_", "DestFolder" }, { 0 } },
};

// The standard actions a module's tables need, at their place in Microsoft Learn's suggested
// InstallExecuteSequence (the package that merges the module has the others).
static const char *const module_actions[] = {
    "RemoveRegistryValues", "RemoveIniValues", "RemoveEnvironmentStrings", "RemoveDuplicateFiles", "RemoveFiles",
    "RemoveFolders", "CreateFolders", "InstallFiles", "DuplicateFiles", "WriteRegistryValues", "WriteIniValues",
    "WriteEnvironmentStrings", NULL,
};

// Column types as msi.dll writes them (src/msi/lower.c): key, nullable, string width or short.
static const rp_msi_wcolumn_t modsig_cols[] = { { "ModuleID", 0x2D48 }, { "Language", 0x2502 }, { "Version", 0x0D20 } };
static const rp_msi_wcolumn_t modcomp_cols[] = { { "Component", 0x2D48 }, { "ModuleID", 0x2D48 }, { "Language", 0x2502 } };
static const rp_msi_wcolumn_t modseq_cols[] = { { "Action", 0x2D40 }, { "Sequence", 0x1502 }, { "BaseAction", 0x1D40 },
                                                { "After", 0x1502 }, { "Condition", 0x1DFF } };

static rp_msi_cell_t *cells(mod_t *m, size_t n) {
    rp_msi_cell_t *c = keep(m, rp_mem_alloc(m->alloc, n ? n : 1, sizeof *c));
    if (c) memset(c, 0, (n ? n : 1) * sizeof *c);
    return c;
}

proven_err_t rp_msm_write(proven_allocator_t alloc, const rp_ir_t *ir, const rp_msi_wtable_t *tables, size_t count,
                          const rp_cab_file_t *files, size_t nfiles, size_t jobs, const rp_limits_t *limits,
                          const rp_out_sink_t *sink, uint8_t **out, size_t *len, const char **why) {
    mod_t m = { .alloc = alloc };
    *why = NULL;
    // ".5A1B2C3D_1111_4222_8333_944455566677" from "{5A1B2C3D-1111-4222-8333-944455566677}"
    if (ir->upgrade_code == NULL || strlen(ir->upgrade_code) != 38) return PROVEN_ERR_INVALID_ARG;
    m.sfx[0] = '.';
    for (size_t i = 0; i < 36; ++i) m.sfx[1 + i] = ir->upgrade_code[1 + i] == '-' ? '_' : ir->upgrade_code[1 + i];
    m.sfx[37] = '\0';

    // The keys the module defines: its own rows' keys, but not TARGETDIR and the standard folders
    // (rows with DefaultDir ".").
    for (size_t t = 0; t < count; ++t) {
        for (size_t r = 0; r < sizeof kept_tables / sizeof kept_tables[0]; ++r) {
            const keep_rule_t *k = &kept_tables[r];
            if (k->own == NULL || strcmp(tables[t].name, k->table) != 0) continue;
            int own = column_of(&tables[t], k->own), dd = column_of(&tables[t], "DefaultDir");
            for (size_t row = 0; own >= 0 && row < tables[t].row_count; ++row) {
                const rp_msi_cell_t *c = &tables[t].cells[row * tables[t].column_count];
                if (dd >= 0 && (c[dd].kind != RP_MSI_STR || (c[dd].len == 1 && c[dd].bytes[0] == '.') ||
                                (c[dd].len == 9 && memcmp(c[dd].bytes, "SourceDir", 9) == 0))) {
                    continue;
                }
                add_key(&m, &c[own]);
            }
        }
    }
    if (m.nkeys) rp_sort(m.keys, m.nkeys, sizeof *m.keys, cmp_str);

    enum { MAXT = 24 };
    rp_msi_wtable_t mt[MAXT];
    size_t nt = 0;
    const rp_msi_wtable_t *component = NULL, *iexec = NULL;
    proven_err_t err = PROVEN_OK;
    for (size_t t = 0; t < count && err == PROVEN_OK; ++t) {
        const rp_msi_wtable_t *src = &tables[t];
        if (strcmp(src->name, "InstallExecuteSequence") == 0) iexec = src;
        const keep_rule_t *k = NULL;
        for (size_t r = 0; r < sizeof kept_tables / sizeof kept_tables[0]; ++r) {
            if (strcmp(src->name, kept_tables[r].table) == 0) k = &kept_tables[r];
        }
        // ICEM04: a module has FeatureComponents and InstallExecuteSequence, both empty.
        if (strcmp(src->name, "FeatureComponents") == 0 || strcmp(src->name, "InstallExecuteSequence") == 0) {
            mt[nt] = *src;
            mt[nt].cells = NULL;
            mt[nt++].row_count = 0;
            continue;
        }
        if (k == NULL || (src->row_count == 0 && strcmp(src->name, "Directory") != 0)) continue;
        rp_msi_cell_t *c = cells(&m, src->row_count * src->column_count);
        if (c == NULL) break;
        for (size_t i = 0; i < src->row_count * src->column_count; ++i) {
            size_t col = i % src->column_count;
            const char *name = src->columns[col].name;
            bool key = k->own && strcmp(name, k->own) == 0, fmt = false;
            for (int j = 0; j < 4 && k->refs[j]; ++j) key |= strcmp(name, k->refs[j]) == 0;
            for (int j = 0; j < 3 && k->fmt[j]; ++j) fmt |= strcmp(name, k->fmt[j]) == 0;
            c[i] = key ? renamed(&m, &src->cells[i]) : fmt ? formatted(&m, &src->cells[i]) : src->cells[i];
        }
        mt[nt] = *src;
        mt[nt].cells = c;
        if (strcmp(src->name, "Component") == 0) component = &mt[nt];
        ++nt;
    }

    // ModuleSignature, ModuleComponents, ModuleInstallExecuteSequence.
    char *module_id = keep(&m, rp_mem_alloc(alloc, strlen(ir->name) + 40, 1));
    if (module_id) snprintf(module_id, strlen(ir->name) + 40, "%s%s", ir->name, m.sfx);
    rp_msi_cell_t *sig = cells(&m, 3);
    if (err == PROVEN_OK && sig && module_id) {
        sig[0] = str_cell(module_id);
        sig[1] = (rp_msi_cell_t){ .kind = RP_MSI_INT, .i = ir->language };
        sig[2] = str_cell(ir->version);
        mt[nt++] = (rp_msi_wtable_t){ "ModuleSignature", modsig_cols, 3, sig, 1 };
    }
    size_t ncomp = component ? component->row_count : 0;
    rp_msi_cell_t *mc = cells(&m, ncomp * 3);
    if (err == PROVEN_OK && mc && module_id) {
        int cc = column_of(component, "Component");
        for (size_t r = 0; r < ncomp; ++r) {
            mc[3 * r] = component->cells[r * component->column_count + (size_t)cc];
            mc[3 * r + 1] = str_cell(module_id);
            mc[3 * r + 2] = (rp_msi_cell_t){ .kind = RP_MSI_INT, .i = ir->language };
        }
        mt[nt++] = (rp_msi_wtable_t){ "ModuleComponents", modcomp_cols, 3, mc, ncomp };
    }
    size_t nseq = 0;
    rp_msi_cell_t *ms = cells(&m, (iexec ? iexec->row_count : 0) * 5);
    if (err == PROVEN_OK && ms && iexec) {
        int a = column_of(iexec, "Action"), cond = column_of(iexec, "Condition"), seq = column_of(iexec, "Sequence");
        for (size_t r = 0; r < iexec->row_count; ++r) {
            const rp_msi_cell_t *row = &iexec->cells[r * iexec->column_count];
            bool wanted = false;
            for (size_t j = 0; module_actions[j] && !wanted; ++j) {
                wanted = row[a].len == strlen(module_actions[j]) && memcmp(row[a].bytes, module_actions[j], row[a].len) == 0;
            }
            if (!wanted) continue;
            ms[5 * nseq] = row[a];
            ms[5 * nseq + 1] = row[seq];
            ms[5 * nseq + 2] = (rp_msi_cell_t){ .kind = RP_MSI_NULL };
            ms[5 * nseq + 3] = (rp_msi_cell_t){ .kind = RP_MSI_NULL };
            ms[5 * nseq + 4] = row[cond];
            ++nseq;
        }
        mt[nt++] = (rp_msi_wtable_t){ "ModuleInstallExecuteSequence", modseq_cols, 5, ms, nseq };
    }
    if (err == PROVEN_OK && m.nomem) err = PROVEN_ERR_NOMEM;

    // The cabinet: the files under their File keys.
    uint8_t *cab = NULL;
    size_t cab_len = 0;
    rp_cab_file_t *cf = nfiles ? rp_mem_alloc(alloc, nfiles, sizeof *cf) : NULL;
    if (err == PROVEN_OK && nfiles && cf == NULL) err = PROVEN_ERR_NOMEM;
    for (size_t i = 0; err == PROVEN_OK && i < nfiles; ++i) {
        char *name = keep(&m, rp_mem_alloc(alloc, strlen(files[i].name) + strlen(m.sfx) + 1, 1));
        if (name == NULL) {
            err = PROVEN_ERR_NOMEM;
            break;
        }
        sprintf(name, "%s%s", files[i].name, m.sfx);
        cf[i] = (rp_cab_file_t){ name, files[i].data, files[i].size };
    }
    if (err == PROVEN_OK && nfiles) err = rp_cab_write_ex(alloc, cf, nfiles, ir->compress, jobs, limits, &cab, &cab_len);
    rp_mem_free(alloc, cf);

    // Summary information, as Microsoft Learn's "Merge Module Summary Information Stream Reference" asks:
    // the module's GUID as the revision number, the platform and language in the template.
    uint8_t *summary = NULL;
    size_t summary_len = 0;
    if (err == PROVEN_OK) {
        char tmpl[32];
        snprintf(tmpl, sizeof tmpl, "%s;%u", ir->arch == RP_ARCH_X64 ? "x64" : ir->arch == RP_ARCH_ARM64 ? "Arm64" : "Intel", ir->language);
        bool ascii = true;
        for (const char *p = ir->manufacturer; p && *p; ++p) ascii &= (unsigned char)*p < 0x80;
        const char *app = "rubrapack " RUBRAPACK_VERSION_STRING;
        struct { uint32_t pid; const char *s; int32_t i; } props[] = {
            { 2, "Merge Module", 0 }, { 3, ir->name, 0 }, { 4, ascii && ir->manufacturer ? ir->manufacturer : "rubrapack", 0 },
            { 7, tmpl, 0 }, { 9, ir->upgrade_code, 0 }, { 14, NULL, ir->arch == RP_ARCH_ARM64 ? 500 : 200 }, { 15, NULL, 2 },
            { 18, app, 0 },
        };
        rp_suminfo_t si = { .count = 0 };
        for (size_t i = 0; i < sizeof props / sizeof props[0]; ++i) {
            rp_suminfo_prop_t *p = &si.props[si.count++];
            p->pid = props[i].pid;
            if (props[i].s) {
                p->type = RP_VT_LPSTR;
                p->str = (const uint8_t *)props[i].s;
                p->str_len = strlen(props[i].s);
            } else {
                p->type = RP_VT_I4;
                p->i = props[i].i;
            }
        }
        err = rp_suminfo_write(&si, false, alloc, &summary, &summary_len);
    }
    if (err == PROVEN_OK) {
        const char *missing = NULL;
        err = rp_msi_validation(alloc, mt, nt, NULL, 0, &mt[nt], &missing);
        if (err == PROVEN_ERR_NOT_FOUND) *why = "internal: a module table has no _Validation rule";
        if (err == PROVEN_OK) {
            rp_msi_wstream_t streams[1] = { { "MergeModule.CABinet", cab, cab_len } };
            rp_msi_wdb_t db = { 65001, mt, nt + 1, summary, summary_len, streams, cab ? 1 : 0 };
            err = rp_msi_write_to(alloc, &db, 9, limits, sink, out, len);
            rp_mem_free(alloc, (void *)mt[nt].cells);
        }
    }
    rp_mem_free(alloc, cab);
    rp_mem_free(alloc, summary);
    for (size_t i = 0; i < m.nkept; ++i) rp_mem_free(alloc, m.kept[i]);
    rp_mem_free(alloc, m.kept);
    rp_mem_free(alloc, m.keys);
    if (err == PROVEN_ERR_NOMEM && *why == NULL) *why = "out of memory";
    return err;
}
