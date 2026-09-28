// src/msi/lint_prev.c - a package against the version before it (RFC-0013 R3, B1): what makes a
// major upgrade fail, and what breaks the component rules.
//
// Errors: another UpgradeCode (the new one does not replace the old), a ProductVersion that is not
// higher in its first three fields (Windows Installer does not upgrade then). Warnings: the same
// ProductCode (a minor upgrade, which rubrapack does not build), a component GUID whose key path,
// folder or 64-bit flag changed (the component rules want a new GUID), a component or a feature
// that is gone.

#include "rubrapack/lint.h"
#include "rubrapack/mem.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    const rp_msi_wdb_t *db;
    char              **strs;       // copies handed out by str(), freed at the end
    size_t              nstr, cap;
    proven_allocator_t  alloc;
    bool                nomem;
} side_t;

static const rp_msi_wtable_t *table(const rp_msi_wdb_t *db, const char *name) {
    for (size_t i = 0; i < db->table_count; ++i) {
        if (strcmp(db->tables[i].name, name) == 0) return &db->tables[i];
    }
    return NULL;
}

static size_t column(const rp_msi_wtable_t *t, const char *name) {
    for (size_t i = 0; t && i < t->column_count; ++i) {
        if (strcmp(t->columns[i].name, name) == 0) return i;
    }
    return SIZE_MAX;
}

static const rp_msi_cell_t *cellp(const rp_msi_wtable_t *t, size_t row, size_t col) {
    return col == SIZE_MAX ? NULL : &t->cells[row * t->column_count + col];
}

// A string cell as a NUL-terminated copy ("" for null).
static const char *str(side_t *s, const rp_msi_cell_t *c) {
    if (c == NULL || c->kind != RP_MSI_STR) return "";
    if (s->nstr == s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 256;
        char **v = rp_mem_alloc(s->alloc, cap, sizeof *v);
        if (v == NULL) {
            s->nomem = true;
            return "";
        }
        if (s->nstr) memcpy(v, s->strs, s->nstr * sizeof *v);
        rp_mem_free(s->alloc, s->strs);
        s->strs = v;
        s->cap = cap;
    }
    char *p = rp_mem_alloc(s->alloc, c->len + 1, 1);
    if (p == NULL) {
        s->nomem = true;
        return "";
    }
    memcpy(p, c->bytes, c->len);
    p[c->len] = 0;
    s->strs[s->nstr++] = p;
    return p;
}

// Compares a string cell with a string, or two cells (case-insensitively for GUIDs), without copies.
static bool is(const rp_msi_cell_t *c, const char *v) {
    size_t n = strlen(v);
    if (c == NULL || c->kind != RP_MSI_STR) return n == 0;
    return c->len == n && memcmp(c->bytes, v, n) == 0;
}

static bool same_guid(const rp_msi_cell_t *a, const rp_msi_cell_t *b) {
    if (a == NULL || b == NULL || a->kind != RP_MSI_STR || b->kind != RP_MSI_STR || a->len != b->len) return false;
    for (size_t i = 0; i < a->len; ++i) {
        uint8_t x = a->bytes[i], y = b->bytes[i];
        if (x >= 'a' && x <= 'z') x = (uint8_t)(x - 32);
        if (y >= 'a' && y <= 'z') y = (uint8_t)(y - 32);
        if (x != y) return false;
    }
    return true;
}

static const char *property(side_t *s, const char *name) {
    const rp_msi_wtable_t *t = table(s->db, "Property");
    size_t kp = column(t, "Property"), kv = column(t, "Value");
    for (size_t r = 0; t && r < t->row_count; ++r) {
        if (is(cellp(t, r, kp), name)) return str(s, cellp(t, r, kv));
    }
    return "";
}

// The long name of a "short|long" (or plain) name.
static const char *long_name(const char *n) {
    const char *bar = strchr(n, '|');
    return bar ? bar + 1 : n;
}

// snprintf that may cut a long path short (only compared and printed).
#define CUT(out, cap, ...)                                                   \
    do {                                                                     \
        if (snprintf((out), (cap), __VA_ARGS__) >= (int)(cap)) (out)[(cap) - 1] = 0; \
    } while (0)

// A directory as a path of long names from its root key: "ProgramFiles64Folder/Example/bin".
static void dir_path(side_t *s, const char *key, char *out, size_t cap, int depth) {
    out[0] = 0;
    const rp_msi_wtable_t *t = table(s->db, "Directory");
    size_t kd = column(t, "Directory"), kp = column(t, "Directory_Parent"), kn = column(t, "DefaultDir");
    for (size_t r = 0; t && r < t->row_count; ++r) {
        if (!is(cellp(t, r, kd), key)) continue;
        const char *parent = str(s, cellp(t, r, kp));
        char dflt[512];
        CUT(dflt, sizeof dflt, "%s", str(s, cellp(t, r, kn)));
        char *colon = strchr(dflt, ':');       // target:source - the target part
        if (colon) *colon = 0;
        const char *name = long_name(dflt);
        if (parent[0] == 0 || strcmp(parent, key) == 0 || depth > 64) {
            CUT(out, cap, "%s", key);
            return;
        }
        char up[1024];
        dir_path(s, parent, up, sizeof up, depth + 1);
        if (strcmp(name, ".") == 0) CUT(out, cap, "%s", strcmp(up, "TARGETDIR") == 0 ? key : up);
        else CUT(out, cap, "%s/%s", strcmp(up, "TARGETDIR") == 0 ? key : up, name);
        return;
    }
    CUT(out, cap, "%s", key);
}

// What a component's key path points at: a file, a registry value, or its folder.
static void key_path(side_t *s, size_t row, char *out, size_t cap) {
    const rp_msi_wtable_t *c = table(s->db, "Component");
    int32_t attr = cellp(c, row, column(c, "Attributes"))->i;
    const char *kp = str(s, cellp(c, row, column(c, "KeyPath")));
    char dir[1024];
    dir_path(s, str(s, cellp(c, row, column(c, "Directory_"))), dir, sizeof dir, 0);
    if (kp[0] == 0) {
        CUT(out, cap, "folder %s", dir);
    } else if (attr & 4) {
        const rp_msi_wtable_t *t = table(s->db, "Registry");
        size_t kk = column(t, "Registry");
        for (size_t r = 0; t && r < t->row_count; ++r) {
            if (!is(cellp(t, r, kk), kp)) continue;
            CUT(out, cap, "registry %d\\%s\\%s", cellp(t, r, column(t, "Root"))->i, str(s, cellp(t, r, column(t, "Key"))),
                     str(s, cellp(t, r, column(t, "Name"))));
            return;
        }
        CUT(out, cap, "registry %s", kp);
    } else {
        const rp_msi_wtable_t *t = table(s->db, "File");
        size_t kf = column(t, "File");
        for (size_t r = 0; t && r < t->row_count; ++r) {
            if (!is(cellp(t, r, kf), kp)) continue;
            CUT(out, cap, "file %s/%s", dir, long_name(str(s, cellp(t, r, column(t, "FileName")))));
            return;
        }
        CUT(out, cap, "file %s", kp);
    }
}

static int version_cmp(const char *a, const char *b) {
    unsigned va[3] = { 0 }, vb[3] = { 0 };
    sscanf(a, "%u.%u.%u", &va[0], &va[1], &va[2]);
    sscanf(b, "%u.%u.%u", &vb[0], &vb[1], &vb[2]);
    for (int i = 0; i < 3; ++i) {
        if (va[i] != vb[i]) return va[i] < vb[i] ? -1 : 1;
    }
    return 0;
}

static bool guid_eq(const char *a, const char *b) {
    size_t n = strlen(a);
    if (n != strlen(b)) return false;
    for (size_t i = 0; i < n; ++i) {
        char x = a[i] >= 'a' && a[i] <= 'z' ? (char)(a[i] - 32) : a[i], y = b[i] >= 'a' && b[i] <= 'z' ? (char)(b[i] - 32) : b[i];
        if (x != y) return false;
    }
    return true;
}

#define WARN(...) rp_srcdiag_add(diags, (rp_pos_t){ 0 }, code, true, __VA_ARGS__)

proven_err_t rp_msi_lint_previous(proven_allocator_t alloc, const rp_msi_wdb_t *now, const rp_msi_wdb_t *before, rp_srcdiags_t *diags) {
    if (now == NULL || before == NULL || diags == NULL) return PROVEN_ERR_INVALID_ARG;
    side_t n = { .db = now, .alloc = alloc }, o = { .db = before, .alloc = alloc };
    const char *code;
    // The upgrade itself.
    if (!guid_eq(property(&n, "UpgradeCode"), property(&o, "UpgradeCode"))) {
        rp_srcdiag_add(diags, (rp_pos_t){ 0 }, "RP2301", false, "UpgradeCode %s differs from the previous version's %s: this package does not replace it",
                       property(&n, "UpgradeCode"), property(&o, "UpgradeCode"));
    }
    const char *vn = property(&n, "ProductVersion"), *vo = property(&o, "ProductVersion");
    if (version_cmp(vn, vo) <= 0) {
        rp_srcdiag_add(diags, (rp_pos_t){ 0 }, "RP2302", false,
                       "ProductVersion %s is not higher than the previous %s in its first three fields: Windows Installer does not upgrade", vn, vo);
    }
    if (guid_eq(property(&n, "ProductCode"), property(&o, "ProductCode"))) {
        code = "RP2303";
        WARN("the ProductCode is the previous version's: that is a minor upgrade (REINSTALLMODE), not the major upgrade rubrapack builds");
    }
    // The component rules: one GUID, one key path in one folder, one bitness - for ever.
    const rp_msi_wtable_t *cn = table(now, "Component"), *co = table(before, "Component");
    size_t gn = column(cn, "ComponentId"), go = column(co, "ComponentId");
    for (size_t ro = 0; co && ro < co->row_count; ++ro) {
        const rp_msi_cell_t *gc = cellp(co, ro, go);
        if (gc == NULL || gc->kind != RP_MSI_STR) continue;
        size_t rn = SIZE_MAX;
        for (size_t r = 0; cn && r < cn->row_count && rn == SIZE_MAX; ++r) {
            if (same_guid(cellp(cn, r, gn), gc)) rn = r;
        }
        const char *g = str(&o, gc);
        const char *name = str(&o, cellp(co, ro, column(co, "Component")));
        if (rn == SIZE_MAX) {
            code = "RP2306";
            WARN("component %s %s is gone (its resources are removed with the previous version)", name, g);
            continue;
        }
        char a[1400], b[1400];
        key_path(&o, ro, a, sizeof a);
        key_path(&n, rn, b, sizeof b);
        if (strcmp(a, b) != 0) {
            code = "RP2304";
            WARN("component %s %s now has the key path %s instead of %s: the component rules want a new GUID", name, g, b, a);
        }
        const rp_msi_cell_t *xo = cellp(co, ro, column(co, "Attributes")), *xn = cellp(cn, rn, column(cn, "Attributes"));
        if (xo && xn && (xo->i & 256) != (xn->i & 256)) {
            code = "RP2305";
            WARN("component %s %s changed between 32 and 64 bits: the component rules want a new GUID", name, g);
        }
    }
    // Features the user may have chosen, and that are gone.
    const rp_msi_wtable_t *fn = table(now, "Feature"), *fo = table(before, "Feature");
    for (size_t ro = 0; fo && ro < fo->row_count; ++ro) {
        const rp_msi_cell_t *fc = cellp(fo, ro, column(fo, "Feature"));
        bool found = false;
        for (size_t r = 0; fn && r < fn->row_count && !found; ++r) {
            const rp_msi_cell_t *x = cellp(fn, r, column(fn, "Feature"));
            found = x && fc && x->kind == fc->kind && x->len == fc->len && memcmp(x->bytes, fc->bytes, fc->len) == 0;
        }
        const char *f = str(&o, fc);
        if (!found) {
            code = "RP2307";
            WARN("feature %s is gone: installations that had it lose what it held", f);
        }
    }
    bool nomem = n.nomem || o.nomem;
    for (size_t i = 0; i < n.nstr; ++i) rp_mem_free(alloc, n.strs[i]);
    for (size_t i = 0; i < o.nstr; ++i) rp_mem_free(alloc, o.strs[i]);
    rp_mem_free(alloc, n.strs);
    rp_mem_free(alloc, o.strs);
    return nomem ? PROVEN_ERR_NOMEM : PROVEN_OK;
}
