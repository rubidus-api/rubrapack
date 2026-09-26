// src/msi/lint.c - basic lint over a finished table set (include/rubrapack/lint.h).
//
// Generic rules come from the column types (_Columns.Type bits); table rules come from the
// meaning of the standard tables on Microsoft Learn. Keys are looked up through a small hash
// index so a package with many files stays linear.

#include "rubrapack/lint.h"
#include "rubrapack/mem.h"
#include "rubrapack/suminfo.h"
#include "rubrapack/text.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    proven_allocator_t   alloc;
    const rp_msi_wdb_t  *db;
    rp_srcdiags_t       *diags;
    size_t               errors;
    bool                 nomem;
} lint_t;

static rp_pos_t nopos(void) { return (rp_pos_t){ 0, 0 }; }

// ---- tables and cells ------------------------------------------------------------------------

static const rp_msi_wtable_t *table(const lint_t *l, const char *name) {
    for (size_t i = 0; i < l->db->table_count; ++i) {
        if (strcmp(l->db->tables[i].name, name) == 0) return &l->db->tables[i];
    }
    return NULL;
}

static size_t column(const rp_msi_wtable_t *t, const char *name) {
    for (size_t c = 0; c < t->column_count; ++c) {
        if (strcmp(t->columns[c].name, name) == 0) return c;
    }
    return SIZE_MAX;
}

static const rp_msi_cell_t *cell(const rp_msi_wtable_t *t, size_t row, size_t col) {
    return &t->cells[row * t->column_count + col];
}

// An empty string is stored as null (msi.dll does the same).
static bool is_null(const rp_msi_cell_t *c) { return c->kind == RP_MSI_NULL || (c->kind == RP_MSI_STR && c->len == 0); }

static size_t key_count(const rp_msi_wtable_t *t) {
    size_t n = 0;
    while (n < t->column_count && (t->columns[n].type & RP_MSI_COL_KEY)) ++n;
    return n;
}

// The key cells as text for messages (`'A'/'B'` for a two-column key), or the row number.
static void row_name(const rp_msi_wtable_t *t, size_t row, char *out, size_t cap) {
    size_t n = key_count(t), used = 0;
    if (n == 0) {
        snprintf(out, cap, "#%zu", row + 1);
        return;
    }
    out[0] = '\0';
    for (size_t k = 0; k < n && used < cap; ++k) {
        const rp_msi_cell_t *c = cell(t, row, k);
        const char *sep = k ? "/" : "";
        int w;
        if (is_null(c)) w = snprintf(out + used, cap - used, "%snull", sep);
        else if (c->kind == RP_MSI_INT) w = snprintf(out + used, cap - used, "%s%ld", sep, (long)c->i);
        else w = snprintf(out + used, cap - used, "%s'%.*s'", sep, (int)(c->len > 60 ? 60 : c->len), (const char *)c->bytes);
        if (w < 0) break;
        used += (size_t)w;
    }
}

[[gnu::format(RP_PRINTF_FORMAT, 5, 6)]]
static void finding(lint_t *l, const char *code, const rp_msi_wtable_t *t, size_t row, const char *fmt, ...) {
    char what[200], where[200] = "";
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(what, sizeof what, fmt, ap);
    va_end(ap);
    if (t && row != SIZE_MAX) {
        char name[160];
        row_name(t, row, name, sizeof name);
        snprintf(where, sizeof where, "%s row %s: ", t->name, name);
    } else if (t) {
        snprintf(where, sizeof where, "%s: ", t->name);
    }
    rp_srcdiag_add(l->diags, nopos(), code, false, "lint: %s%s", where, what);
    ++l->errors;
}

static bool str_is(const rp_msi_cell_t *c, const char *s) {
    return c->kind == RP_MSI_STR && c->len == strlen(s) && memcmp(c->bytes, s, c->len) == 0;
}

// ---- key index -------------------------------------------------------------------------------

typedef struct {
    const rp_msi_wtable_t *t;
    size_t                 nkeys;
    size_t                *slot;       // row + 1, 0 = empty
    size_t                 cap;        // power of two
} index_t;

static uint64_t hash_cells(const rp_msi_cell_t *const *cells, size_t n) {
    uint64_t h = 1469598103934665603u;
    for (size_t k = 0; k < n; ++k) {
        const rp_msi_cell_t *c = cells[k];
        bool null = is_null(c);
        h = (h ^ (uint64_t)(null ? 0 : c->kind)) * 1099511628211u;
        if (null) continue;
        if (c->kind == RP_MSI_INT) {
            h = (h ^ (uint32_t)c->i) * 1099511628211u;
        } else {
            for (size_t i = 0; i < c->len; ++i) h = (h ^ c->bytes[i]) * 1099511628211u;
        }
    }
    return h;
}

static bool cell_eq(const rp_msi_cell_t *a, const rp_msi_cell_t *b) {
    bool an = is_null(a), bn = is_null(b);
    if (an || bn) return an == bn;
    if (a->kind != b->kind) return false;
    if (a->kind == RP_MSI_INT) return a->i == b->i;
    return a->len == b->len && memcmp(a->bytes, b->bytes, a->len) == 0;
}

// Finds the row whose leading key cells equal `cells`; SIZE_MAX when there is none.
static size_t index_find(const index_t *x, const rp_msi_cell_t *const *cells) {
    if (x->slot == NULL) return SIZE_MAX;
    for (size_t s = (size_t)hash_cells(cells, x->nkeys) & (x->cap - 1);; s = (s + 1) & (x->cap - 1)) {
        if (x->slot[s] == 0) return SIZE_MAX;
        size_t row = x->slot[s] - 1;
        bool eq = true;
        for (size_t k = 0; eq && k < x->nkeys; ++k) eq = cell_eq(cell(x->t, row, k), cells[k]);
        if (eq) return row;
    }
}

static size_t index_find1(const index_t *x, const rp_msi_cell_t *c) {
    const rp_msi_cell_t *cells[1] = { c };
    return x->nkeys == 1 ? index_find(x, cells) : SIZE_MAX;
}

// Builds the index; reports `code` for rows whose key is already there.
static void index_build(lint_t *l, const rp_msi_wtable_t *t, index_t *x, const char *code, const char *what) {
    *x = (index_t){ .t = t, .nkeys = t ? key_count(t) : 0 };
    if (t == NULL || x->nkeys == 0) return;
    x->cap = 16;
    while (x->cap < t->row_count * 2) x->cap *= 2;
    x->slot = rp_mem_alloc(l->alloc, x->cap, sizeof *x->slot);
    if (x->slot == NULL) {
        l->nomem = true;
        return;
    }
    memset(x->slot, 0, x->cap * sizeof *x->slot);
    const rp_msi_cell_t *cells[32];
    for (size_t row = 0; row < t->row_count; ++row) {
        for (size_t k = 0; k < x->nkeys && k < 32; ++k) cells[k] = cell(t, row, k);
        if (index_find(x, cells) != SIZE_MAX) {
            finding(l, code, t, row, "%s", what);
            continue;
        }
        size_t s = (size_t)hash_cells(cells, x->nkeys) & (x->cap - 1);
        while (x->slot[s]) s = (s + 1) & (x->cap - 1);
        x->slot[s] = row + 1;
    }
}

static void index_free(lint_t *l, index_t *x) { rp_mem_free(l->alloc, x->slot); }

// ---- generic column rules --------------------------------------------------------------------

static void lint_cells(lint_t *l, const rp_msi_wtable_t *t) {
    for (size_t row = 0; row < t->row_count; ++row) {
        for (size_t c = 0; c < t->column_count; ++c) {
            const rp_msi_wcolumn_t *col = &t->columns[c];
            const rp_msi_cell_t *v = cell(t, row, c);
            bool string = col->type & RP_MSI_COL_STRING;
            bool binary = string && !(col->type & RP_MSI_COL_NONBINARY);
            unsigned width = col->type & RP_MSI_COL_WIDTH;
            if (is_null(v)) {
                if (!(col->type & RP_MSI_COL_NULLABLE)) finding(l, "RP2002", t, row, "column %s may not be null", col->name);
                continue;
            }
            uint8_t want = binary ? RP_MSI_BINARY : string ? RP_MSI_STR : RP_MSI_INT;
            if (v->kind != want) {
                finding(l, "RP2001", t, row, "column %s holds the wrong kind of value", col->name);
                continue;
            }
            if (want == RP_MSI_INT) {
                bool ok = width == 2 ? v->i >= -32767 && v->i <= 32767 : v->i != INT32_MIN;
                if (!ok) finding(l, "RP2004", t, row, "column %s: %ld is outside the column's range", col->name, (long)v->i);
            } else if (want == RP_MSI_STR) {
                rp_text_result_t r = rp_utf8_to_utf16(v->bytes, v->len, NULL, 0);
                if (r.err != PROVEN_OK) {
                    finding(l, "RP2005", t, row, "column %s is not valid UTF-8", col->name);
                } else if (width && r.units > width) {
                    finding(l, "RP2003", t, row, "column %s: %zu UTF-16 units, the column holds %u", col->name, r.units, width);
                }
            }
        }
    }
}

// ---- table rules -----------------------------------------------------------------------------

typedef struct {
    const char *table, *column, *target;
} fk_t;

// Single-column references between the standard tables rubrapack writes. Component.KeyPath is
// checked separately (RP2014) because its target depends on the component's attributes.
static const fk_t foreign_keys[] = {
    { "Directory", "Directory_Parent", "Directory" }, { "Component", "Directory_", "Directory" },
    { "Feature", "Feature_Parent", "Feature" },       { "Feature", "Directory_", "Directory" },
    { "FeatureComponents", "Feature_", "Feature" },   { "FeatureComponents", "Component_", "Component" },
    { "File", "Component_", "Component" },            { "CreateFolder", "Directory_", "Directory" },
    { "CreateFolder", "Component_", "Component" },    { "MsiFileHash", "File_", "File" },
    { "RemoveFile", "Component_", "Component" },      { "Registry", "Component_", "Component" },
    { "RemoveRegistry", "Component_", "Component" },
    { "DuplicateFile", "Component_", "Component" },   { "DuplicateFile", "File_", "File" },
    { "DuplicateFile", "DestFolder", "Directory" },   { "RemoveFile", "DirProperty", "Directory" },
    { "Shortcut", "Component_", "Component" },        { "Shortcut", "Directory_", "Directory" },
};

typedef struct {
    const char *name;
    index_t     x;
} named_index_t;

static const index_t *find_index(named_index_t *ix, size_t n, const char *name) {
    for (size_t i = 0; i < n; ++i) {
        if (strcmp(ix[i].name, name) == 0) return &ix[i].x;
    }
    return NULL;
}

static void lint_foreign_keys(lint_t *l, named_index_t *ix, size_t nix) {
    for (size_t f = 0; f < sizeof foreign_keys / sizeof foreign_keys[0]; ++f) {
        const rp_msi_wtable_t *t = table(l, foreign_keys[f].table);
        if (t == NULL) continue;
        size_t c = column(t, foreign_keys[f].column);
        if (c == SIZE_MAX) continue;
        const index_t *target = find_index(ix, nix, foreign_keys[f].target);
        for (size_t row = 0; row < t->row_count; ++row) {
            const rp_msi_cell_t *v = cell(t, row, c);
            if (is_null(v)) continue;
            // A Directory row naming itself as parent is the root (TARGETDIR); msi.dll accepts both.
            if (t == table(l, "Directory") && c == column(t, "Directory_Parent") && cell_eq(v, cell(t, row, 0))) continue;
            if (target == NULL || index_find1(target, v) == SIZE_MAX) {
                finding(l, "RP2007", t, row, "%s '%.*s' is not in %s", foreign_keys[f].column, (int)v->len,
                        (const char *)v->bytes, foreign_keys[f].target);
            }
        }
    }
}

static bool is_guid(const rp_msi_cell_t *v) {
    static const char pattern[] = "{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}";
    if (v->kind != RP_MSI_STR || v->len != sizeof pattern - 1) return false;
    for (size_t i = 0; i < v->len; ++i) {
        char p = pattern[i], ch = (char)v->bytes[i];
        if (p == 'X' ? !((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'F')) : ch != p) return false;
    }
    return true;
}

static const rp_msi_cell_t *property(const index_t *props, const char *name) {
    if (props == NULL || props->slot == NULL) return NULL;
    rp_msi_cell_t key = { .kind = RP_MSI_STR, .bytes = (const uint8_t *)name, .len = strlen(name) };
    size_t row = index_find1(props, &key);
    return row == SIZE_MAX ? NULL : cell(props->t, row, 1);
}

static void lint_properties(lint_t *l, const index_t *props) {
    const rp_msi_wtable_t *t = table(l, "Property");
    static const char *const required[] = { "ProductCode", "ProductName", "ProductVersion", "Manufacturer", "ProductLanguage" };
    for (size_t i = 0; i < sizeof required / sizeof required[0]; ++i) {
        const rp_msi_cell_t *v = property(props, required[i]);
        if (v == NULL || is_null(v)) finding(l, "RP2010", t, SIZE_MAX, "required property %s is missing", required[i]);
    }
    static const char *const guids[] = { "ProductCode", "UpgradeCode" };
    for (size_t i = 0; i < 2; ++i) {
        const rp_msi_cell_t *v = property(props, guids[i]);
        if (v && !is_null(v) && !is_guid(v)) finding(l, "RP2008", t, SIZE_MAX, "%s is not a GUID in registry format", guids[i]);
    }
    const rp_msi_cell_t *pc = property(props, "ProductCode"), *uc = property(props, "UpgradeCode");
    if (pc && uc && cell_eq(pc, uc)) finding(l, "RP2008", t, SIZE_MAX, "ProductCode and UpgradeCode are the same GUID");
}

static void lint_components(lint_t *l, const index_t *files, const index_t *regs) {
    const rp_msi_wtable_t *t = table(l, "Component");
    if (t == NULL) return;
    size_t cid = column(t, "ComponentId"), attr = column(t, "Attributes"), kp = column(t, "KeyPath");
    if (cid == SIZE_MAX || attr == SIZE_MAX || kp == SIZE_MAX) return;
    // Duplicate ComponentIds: index a one-column view of the ComponentId column; a finding names
    // the GUID, which `inspect <msi> Component` then locates.
    rp_msi_wcolumn_t idcol = { "ComponentId", RP_MSI_COL_KEY | RP_MSI_COL_STRING | RP_MSI_COL_NONBINARY | 38 };
    rp_msi_cell_t *ids = rp_mem_alloc(l->alloc, t->row_count ? t->row_count : 1, sizeof *ids);
    if (ids == NULL) {
        l->nomem = true;
        return;
    }
    size_t nid = 0;
    for (size_t row = 0; row < t->row_count; ++row) {
        const rp_msi_cell_t *v = cell(t, row, cid);
        if (is_null(v)) continue;           // an unmanaged component; allowed
        if (!is_guid(v)) finding(l, "RP2008", t, row, "ComponentId is not a GUID in registry format");
        ids[nid++] = *v;
    }
    rp_msi_wtable_t view = { "Component.ComponentId", &idcol, 1, ids, nid };
    index_t x;
    index_build(l, &view, &x, "RP2009", "used by more than one component");
    index_free(l, &x);
    rp_mem_free(l->alloc, ids);

    // Key paths: a file key path must be a file of the same component.
    const rp_msi_wtable_t *ft = table(l, "File");
    size_t fcomp = ft ? column(ft, "Component_") : SIZE_MAX;
    for (size_t row = 0; row < t->row_count; ++row) {
        const rp_msi_cell_t *v = cell(t, row, kp), *a = cell(t, row, attr);
        int32_t flags = a->kind == RP_MSI_INT ? a->i : 0;
        if (!is_null(v) && (flags & 0x4)) {                     // registry key path: a Registry row of this component
            const rp_msi_wtable_t *rt = table(l, "Registry");
            size_t rrow = regs ? index_find1(regs, v) : SIZE_MAX;
            size_t rcomp = rt ? column(rt, "Component_") : SIZE_MAX;
            if (rrow == SIZE_MAX || rcomp == SIZE_MAX) {
                finding(l, "RP2014", t, row, "KeyPath '%.*s' is not in Registry", (int)v->len, (const char *)v->bytes);
            } else if (!cell_eq(cell(rt, rrow, rcomp), cell(t, row, 0))) {
                finding(l, "RP2014", t, row, "KeyPath '%.*s' belongs to another component", (int)v->len, (const char *)v->bytes);
            }
            continue;
        }
        if (is_null(v) || (flags & 0x20)) continue;             // folder or ODBC key path
        size_t frow = files ? index_find1(files, v) : SIZE_MAX;
        if (frow == SIZE_MAX || fcomp == SIZE_MAX) {
            finding(l, "RP2014", t, row, "KeyPath '%.*s' is not in File", (int)v->len, (const char *)v->bytes);
        } else if (!cell_eq(cell(ft, frow, fcomp), cell(t, row, 0))) {
            finding(l, "RP2014", t, row, "KeyPath '%.*s' belongs to another component", (int)v->len, (const char *)v->bytes);
        }
    }
}

static void lint_media(lint_t *l) {
    const rp_msi_wtable_t *ft = table(l, "File"), *mt = table(l, "Media");
    if (ft == NULL || ft->row_count == 0) return;
    size_t seq = column(ft, "Sequence");
    if (seq == SIZE_MAX) return;
    if (mt == NULL || mt->row_count == 0) {
        finding(l, "RP2011", ft, SIZE_MAX, "files but no Media row");
        return;
    }
    size_t disk = column(mt, "DiskId"), last = column(mt, "LastSequence");
    if (disk == SIZE_MAX || last == SIZE_MAX) return;
    int32_t max_last = 0;
    // Rows are unordered here; check LastSequence grows with DiskId by comparing every pair.
    for (size_t r = 0; r < mt->row_count; ++r) {
        int32_t d = cell(mt, r, disk)->i, s = cell(mt, r, last)->i;
        if (s > max_last) max_last = s;
        for (size_t q = 0; q < mt->row_count; ++q) {
            if (cell(mt, q, disk)->i < d && cell(mt, q, last)->i > s) {
                finding(l, "RP2011", mt, r, "LastSequence %ld is below that of a lower DiskId", (long)s);
                break;
            }
        }
    }
    // File.Sequence: every value in 1..max LastSequence, no two files alike.
    uint8_t *seen = rp_mem_alloc(l->alloc, (size_t)max_last + 1, 1);
    if (seen == NULL) {
        l->nomem = true;
        return;
    }
    memset(seen, 0, (size_t)max_last + 1);
    for (size_t row = 0; row < ft->row_count; ++row) {
        const rp_msi_cell_t *v = cell(ft, row, seq);
        if (v->kind != RP_MSI_INT) continue;
        if (v->i < 1 || v->i > max_last) {
            finding(l, "RP2011", ft, row, "Sequence %ld is not covered by any Media row (last %ld)", (long)v->i, (long)max_last);
        } else if (seen[v->i]++) {
            finding(l, "RP2011", ft, row, "Sequence %ld is used by another file", (long)v->i);
        }
    }
    rp_mem_free(l->alloc, seen);
}

// Standard actions (Microsoft Learn "Standard Actions Reference"); a sequence row names one of
// these, a CustomAction, or (UI sequences) a Dialog.
static const char *const standard_actions[] = {
    "ADMIN", "ADVERTISE", "AllocateRegistrySpace", "AppSearch", "BindImage", "CCPSearch", "CostFinalize",
    "CostInitialize", "CreateFolders", "CreateShortcuts", "DeleteServices", "DisableRollback", "DuplicateFiles",
    "ExecuteAction", "FileCost", "FindRelatedProducts", "ForceReboot", "INSTALL", "InstallAdminPackage",
    "InstallExecute", "InstallExecuteAgain", "InstallFiles", "InstallFinalize", "InstallInitialize",
    "InstallODBC", "InstallServices", "InstallSFPCatalogFile", "InstallValidate", "IsolateComponents",
    "LaunchConditions", "MigrateFeatureStates", "MoveFiles", "MsiConfigureServices", "MsiPublishAssemblies",
    "MsiUnpublishAssemblies", "PatchFiles", "ProcessComponents", "PublishComponents", "PublishFeatures",
    "PublishProduct", "RegisterClassInfo", "RegisterComPlus", "RegisterExtensionInfo", "RegisterFonts",
    "RegisterMIMEInfo", "RegisterProduct", "RegisterProgIdInfo", "RegisterTypeLibraries", "RegisterUser",
    "RemoveDuplicateFiles", "RemoveEnvironmentStrings", "RemoveExistingProducts", "RemoveFiles",
    "RemoveFolders", "RemoveIniValues", "RemoveODBC", "RemoveRegistryValues", "RemoveShortcuts", "ResolveSource",
    "RMCCPSearch", "ScheduleReboot", "SelfRegModules", "SelfUnregModules", "SEQUENCE", "SetODBCFolders",
    "StartServices", "StopServices", "UnpublishComponents", "UnpublishFeatures", "UnregisterClassInfo",
    "UnregisterComPlus", "UnregisterExtensionInfo", "UnregisterFonts", "UnregisterMIMEInfo",
    "UnregisterProgIdInfo", "UnregisterTypeLibraries", "ValidateProductID", "WriteEnvironmentStrings",
    "WriteIniValues", "WriteRegistryValues",
};

static bool is_standard(const rp_msi_cell_t *v) {
    for (size_t i = 0; i < sizeof standard_actions / sizeof standard_actions[0]; ++i) {
        if (str_is(v, standard_actions[i])) return true;
    }
    return false;
}

static void lint_sequence(lint_t *l, const char *name, const index_t *cas, const index_t *dialogs) {
    const rp_msi_wtable_t *t = table(l, name);
    if (t == NULL) return;
    size_t act = column(t, "Action"), seq = column(t, "Sequence");
    if (act == SIZE_MAX || seq == SIZE_MAX) return;
    for (size_t row = 0; row < t->row_count; ++row) {
        const rp_msi_cell_t *v = cell(t, row, act);
        if (is_null(v) || is_standard(v)) continue;
        if (cas && index_find1(cas, v) != SIZE_MAX) continue;
        if (dialogs && index_find1(dialogs, v) != SIZE_MAX) continue;
        finding(l, "RP2007", t, row, "action is neither a standard action nor in CustomAction");
    }
    // Order constraints between standard actions that are present (Microsoft Learn: the costing
    // actions come first, file actions run inside the InstallInitialize..InstallFinalize script).
    static const char *const order[][2] = {
        { "CostInitialize", "FileCost" },         { "FileCost", "CostFinalize" },
        { "CostFinalize", "InstallValidate" },    { "InstallValidate", "InstallInitialize" },
        { "InstallInitialize", "ProcessComponents" }, { "InstallInitialize", "InstallFiles" },
        { "InstallInitialize", "RemoveFiles" },   { "RemoveFiles", "InstallFiles" },
        { "RemoveFolders", "CreateFolders" },     { "InstallFiles", "InstallFinalize" },
        { "RegisterProduct", "InstallFinalize" }, { "PublishProduct", "InstallFinalize" },
        { "FindRelatedProducts", "MigrateFeatureStates" }, { "CostFinalize", "MigrateFeatureStates" },
        { "InstallValidate", "RemoveExistingProducts" },
    };
    for (size_t i = 0; i < sizeof order / sizeof order[0]; ++i) {
        size_t a = SIZE_MAX, b = SIZE_MAX;
        for (size_t row = 0; row < t->row_count; ++row) {
            if (str_is(cell(t, row, act), order[i][0])) a = row;
            if (str_is(cell(t, row, act), order[i][1])) b = row;
        }
        if (a == SIZE_MAX || b == SIZE_MAX) continue;
        const rp_msi_cell_t *sa = cell(t, a, seq), *sb = cell(t, b, seq);
        if (sa->kind == RP_MSI_INT && sb->kind == RP_MSI_INT && sa->i >= sb->i) {
            finding(l, "RP2012", t, b, "sequence %ld is not after %s (%ld)", (long)sb->i, order[i][0], (long)sa->i);
        }
    }
}

static void lint_platform(lint_t *l) {
    const rp_msi_wtable_t *t = table(l, "Component");
    if (t == NULL || l->db->summary == NULL) return;
    rp_suminfo_t si;
    if (rp_suminfo_parse(l->db->summary, l->db->summary_len, &si) != PROVEN_OK) return;
    const rp_suminfo_prop_t *tmpl = NULL;
    for (size_t i = 0; i < si.count; ++i) {
        if (si.props[i].pid == 7 && si.props[i].type == RP_VT_LPSTR) tmpl = &si.props[i];
    }
    if (tmpl == NULL) return;
    size_t n = 0;
    while (n < tmpl->str_len && tmpl->str[n] != ';') ++n;
    bool is64;
    if ((n == 3 && memcmp(tmpl->str, "x64", 3) == 0) || (n == 5 && memcmp(tmpl->str, "Arm64", 5) == 0)) is64 = true;
    else if (n == 5 && memcmp(tmpl->str, "Intel", 5) == 0) is64 = false;
    else return;
    size_t attr = column(t, "Attributes");
    if (attr == SIZE_MAX) return;
    for (size_t row = 0; row < t->row_count; ++row) {
        const rp_msi_cell_t *a = cell(t, row, attr);
        bool comp64 = a->kind == RP_MSI_INT && (a->i & 0x100);
        // A 32-bit component in a 64-bit package is legal (32-bit registry view, RFC-0001 9.2);
        // a 64-bit component cannot be installed by a 32-bit package.
        if (comp64 && !is64) {
            finding(l, "RP2013", t, row, "%s component, but the package platform is %.*s", comp64 ? "64-bit" : "32-bit",
                    (int)n, (const char *)tmpl->str);
        }
    }
}

// A token list "A;B;C" contains name?
static bool list_has(const rp_msi_cell_t *list, const uint8_t *name, size_t len) {
    if (list == NULL || list->kind != RP_MSI_STR) return false;
    size_t i = 0;
    while (i <= list->len) {
        size_t j = i;
        while (j < list->len && list->bytes[j] != ';') ++j;
        if (j - i == len && memcmp(list->bytes + i, name, len) == 0) return true;
        i = j + 1;
    }
    return false;
}

static void lint_upgrade(lint_t *l, const index_t *props) {
    const rp_msi_wtable_t *t = table(l, "Upgrade");
    if (t == NULL) return;
    size_t code = column(t, "UpgradeCode"), prop = column(t, "ActionProperty");
    if (code == SIZE_MAX || prop == SIZE_MAX) return;
    const rp_msi_cell_t *secure = property(props, "SecureCustomProperties");
    for (size_t row = 0; row < t->row_count; ++row) {
        if (!is_guid(cell(t, row, code))) finding(l, "RP2008", t, row, "UpgradeCode is not a GUID in registry format");
        const rp_msi_cell_t *p = cell(t, row, prop);
        if (p->kind != RP_MSI_STR) continue;
        bool upper = true;
        for (size_t i = 0; i < p->len; ++i) upper &= !(p->bytes[i] >= 'a' && p->bytes[i] <= 'z');
        if (!upper) finding(l, "RP2015", t, row, "ActionProperty must be a public (upper case) property");
        else if (!list_has(secure, p->bytes, p->len))
            finding(l, "RP2015", t, row, "ActionProperty '%.*s' is not in SecureCustomProperties", (int)p->len,
                    (const char *)p->bytes);
    }
}

// Custom actions (RFC-0003 2): run-from-file sources exist, no asynchronous rollback, and a
// rollback twin named `<Action>Rollback` is scheduled before `<Action>` so it covers it.
static void lint_custom_actions(lint_t *l, const index_t *files, const index_t *cas) {
    const rp_msi_wtable_t *t = table(l, "CustomAction");
    if (t == NULL) return;
    size_t type = column(t, "Type"), source = column(t, "Source");
    if (type == SIZE_MAX || source == SIZE_MAX) return;
    for (size_t row = 0; row < t->row_count; ++row) {
        const rp_msi_cell_t *ty = cell(t, row, type), *src = cell(t, row, source);
        if (ty->kind != RP_MSI_INT) continue;
        int32_t v = ty->i;
        if ((v & 0x30) == 0x10 && (v & 0x0F) != 0x03 && (v & 0x07) != 0x07) {    // source is a File key (not 19/23)
            if (is_null(src) || files == NULL || index_find1(files, src) == SIZE_MAX) {
                finding(l, "RP2018", t, row, "Source must name a File row for type %ld", (long)v);
            }
        }
        if ((v & 0x100) && (v & 0x80)) finding(l, "RP2017", t, row, "a rollback action cannot be asynchronous");
    }
    const rp_msi_wtable_t *seq = table(l, "InstallExecuteSequence");
    if (seq == NULL) return;
    size_t act = column(seq, "Action"), num = column(seq, "Sequence");
    if (act == SIZE_MAX || num == SIZE_MAX) return;
    // RemoveExistingProducts right after InstallInitialize must come before anything that writes
    // to the script; msi.dll stops with error 2613 otherwise (observed).
    int32_t init = -1, rep = -1;
    for (size_t row = 0; row < seq->row_count; ++row) {
        const rp_msi_cell_t *n = cell(seq, row, num);
        if (n->kind != RP_MSI_INT) continue;
        if (str_is(cell(seq, row, act), "InstallInitialize")) init = n->i;
        if (str_is(cell(seq, row, act), "RemoveExistingProducts")) rep = n->i;
    }
    for (size_t row = 0; init >= 0 && rep > init && row < seq->row_count; ++row) {
        const rp_msi_cell_t *n = cell(seq, row, num);
        if (n->kind != RP_MSI_INT || n->i <= init || n->i >= rep) continue;
        size_t ca = index_find1(cas, cell(seq, row, act));
        if (ca != SIZE_MAX && cell(t, ca, type)->kind == RP_MSI_INT && (cell(t, ca, type)->i & 0x400)) {
            finding(l, "RP2019", seq, row, "a script action between InstallInitialize and RemoveExistingProducts (error 2613)");
        }
    }

    for (size_t row = 0; row < seq->row_count; ++row) {
        const rp_msi_cell_t *name = cell(seq, row, act), *n = cell(seq, row, num);
        const char suffix[] = "Rollback";
        size_t sl = sizeof suffix - 1;
        if (name->kind != RP_MSI_STR || name->len <= sl || n->kind != RP_MSI_INT) continue;
        if (memcmp(name->bytes + name->len - sl, suffix, sl) != 0) continue;
        for (size_t other = 0; other < seq->row_count; ++other) {
            const rp_msi_cell_t *o = cell(seq, other, act), *on = cell(seq, other, num);
            if (o->kind == RP_MSI_STR && o->len == name->len - sl && memcmp(o->bytes, name->bytes, o->len) == 0 &&
                on->kind == RP_MSI_INT && on->i <= n->i) {
                finding(l, "RP2016", seq, row, "rollback twin must come before %.*s (%ld <= %ld)", (int)o->len,
                        (const char *)o->bytes, (long)on->i, (long)n->i);
            }
        }
    }
}

// ---- entry -----------------------------------------------------------------------------------

proven_err_t rp_msi_lint(proven_allocator_t alloc, const rp_msi_wdb_t *db, rp_srcdiags_t *diags) {
    if (db == NULL || diags == NULL) return PROVEN_ERR_INVALID_ARG;
    lint_t l = { .alloc = alloc, .db = db, .diags = diags };
    for (size_t i = 0; i < db->table_count; ++i) lint_cells(&l, &db->tables[i]);

    static const char *const indexed[] = { "Property", "Directory", "Component", "Feature", "File", "CustomAction", "Dialog",
                                           "Registry" };
    enum { NIX = sizeof indexed / sizeof indexed[0] };
    named_index_t ix[NIX];
    for (size_t i = 0; i < NIX; ++i) {
        ix[i].name = indexed[i];
        index_build(&l, table(&l, indexed[i]), &ix[i].x, "RP2006", "duplicate primary key");
    }
    // Duplicate keys in every other table too.
    for (size_t i = 0; i < db->table_count; ++i) {
        bool done = false;
        for (size_t k = 0; k < NIX; ++k) done |= strcmp(db->tables[i].name, indexed[k]) == 0;
        if (done) continue;
        index_t x;
        index_build(&l, &db->tables[i], &x, "RP2006", "duplicate primary key");
        index_free(&l, &x);
    }

    const index_t *props = find_index(ix, NIX, "Property");
    const index_t *cas = find_index(ix, NIX, "CustomAction");
    const index_t *dialogs = find_index(ix, NIX, "Dialog");
    lint_foreign_keys(&l, ix, NIX);
    lint_properties(&l, props);
    lint_components(&l, find_index(ix, NIX, "File"), find_index(ix, NIX, "Registry"));
    lint_media(&l);
    static const char *const sequences[] = { "InstallExecuteSequence", "InstallUISequence", "AdminExecuteSequence",
                                             "AdminUISequence", "AdvtExecuteSequence" };
    for (size_t i = 0; i < sizeof sequences / sizeof sequences[0]; ++i) {
        lint_sequence(&l, sequences[i], cas->slot ? cas : NULL, dialogs->slot ? dialogs : NULL);
    }
    lint_platform(&l);
    lint_custom_actions(&l, find_index(ix, NIX, "File"), cas);
    lint_upgrade(&l, props);

    for (size_t i = 0; i < NIX; ++i) index_free(&l, &ix[i].x);
    if (l.nomem) return PROVEN_ERR_NOMEM;
    return l.errors ? PROVEN_ERR_INVALID_STATE : PROVEN_OK;
}
