// src/msi/patch.c - patches (include/rubrapack/patch.h, RFC-0016 3).
//
// A patch is a compound file of class {000C1086-0000-0000-C000-000000000046}. Its root is a small
// database (MsiPatchMetadata, MsiPatchSequence) with the cabinet of new and changed files as a
// stream, and two transforms as substorages, applied in the order summary property 8 names them:
//   RP1   the product transform: every row that differs between the packages, except that existing
//         files keep the base's Sequence and the Media table stays the base's (the cached package
//         still has to find unchanged files where they were);
//   #RP1  the patch transform: the new and changed files get Sequence numbers from 1000 on (the
//         next thousand above the base's) and attribute 0x1000 (msidbFileAttributesPatchAdded), a
//         Media row points at the patch's cabinet, PatchPackage ties the patch code to that disk,
//         PatchFiles runs at 4001, and the Patch and MsiPatchHeaders tables exist (empty: the files
//         are whole, not deltas).
// Checked against a patch made by Microsoft's MsiMsp.exe from the same two packages (T109).

#include "rubrapack/cab.h"
#include "rubrapack/ident.h"
#include "rubrapack/mem.h"
#include "rubrapack/patch.h"
#include "rubrapack/suminfo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { PATCH_ADDED = 0x1000, PATCH_FILES_SEQ = 4001, MAX_OWN = 4096 };

typedef struct {
    proven_allocator_t alloc;
    const rp_limits_t *limits;
    const char        *why;
    char              *table;
    size_t             table_cap;
    void              *own[MAX_OWN];
    size_t             nown;
} pctx_t;

static void *own(pctx_t *p, void *ptr) {
    if (ptr == NULL) return NULL;
    if (p->nown == MAX_OWN) {
        rp_mem_free(p->alloc, ptr);
        return NULL;
    }
    p->own[p->nown++] = ptr;
    return ptr;
}

static void *zalloc(pctx_t *p, size_t n, size_t size) {
    void *v = rp_mem_alloc(p->alloc, n + 1, size);
    if (v) memset(v, 0, (n + 1) * size);
    return own(p, v);
}

static proven_err_t refuse(pctx_t *p, const char *what, size_t what_len, const char *why) {
    p->why = why;
    if (p->table && p->table_cap) snprintf(p->table, p->table_cap, "%.*s", (int)what_len, what);
    return PROVEN_ERR_UNSUPPORTED;
}

static const rp_msi_wtable_t *table(const rp_msi_wdb_t *db, const char *name) {
    for (size_t t = 0; t < db->table_count; ++t) {
        if (strcmp(db->tables[t].name, name) == 0) return &db->tables[t];
    }
    return NULL;
}

static int col(const rp_msi_wtable_t *t, const char *name) {
    for (size_t c = 0; t && c < t->column_count; ++c) {
        if (strcmp(t->columns[c].name, name) == 0) return (int)c;
    }
    return -1;
}

static bool same_str(const rp_msi_cell_t *a, const rp_msi_cell_t *b) {
    size_t an = a->kind == RP_MSI_STR ? a->len : 0, bn = b->kind == RP_MSI_STR ? b->len : 0;
    return an == bn && (an == 0 || memcmp(a->bytes, b->bytes, an) == 0);
}

static rp_msi_cell_t str_cell(const char *s) {
    return (rp_msi_cell_t){ .kind = s && *s ? RP_MSI_STR : RP_MSI_NULL, .bytes = (const uint8_t *)s, .len = s ? strlen(s) : 0 };
}

static rp_msi_cell_t int_cell(int32_t i) { return (rp_msi_cell_t){ .kind = RP_MSI_INT, .i = i }; }

// A string cell as a NUL-terminated copy ("" for null).
static char *cstr(pctx_t *p, const rp_msi_cell_t *c) {
    size_t n = c->kind == RP_MSI_STR ? c->len : 0;
    char *s = zalloc(p, n + 1, 1);
    if (s && n) memcpy(s, c->bytes, n);
    return s;
}

// The row of `t` whose first column is `key`, or NULL.
static const rp_msi_cell_t *row_by_key(const rp_msi_wtable_t *t, const rp_msi_cell_t *key) {
    for (size_t r = 0; t && r < t->row_count; ++r) {
        if (same_str(&t->cells[r * t->column_count], key)) return &t->cells[r * t->column_count];
    }
    return NULL;
}

// Property.Value of `name`, NUL-terminated ("" when there is none).
static char *property(pctx_t *p, const rp_msi_wdb_t *db, const char *name) {
    const rp_msi_wtable_t *t = table(db, "Property");
    rp_msi_cell_t key = str_cell(name);
    const rp_msi_cell_t *row = t && t->column_count >= 2 ? row_by_key(t, &key) : NULL;
    rp_msi_cell_t none = { .kind = RP_MSI_NULL };
    return cstr(p, row ? &row[1] : &none);
}

static void version(const char *s, long v[4]) {
    for (int i = 0; i < 4; ++i) {
        v[i] = 0;
        if (*s) {
            v[i] = strtol(s, (char **)&s, 10);
            if (*s == '.') ++s;
        }
    }
}

static const char *sum_str(pctx_t *p, const rp_msi_wdb_t *db, uint32_t pid) {
    rp_suminfo_t si = { .count = 0 };
    if (db->summary == NULL || rp_suminfo_parse(db->summary, db->summary_len, &si) != PROVEN_OK) return "";
    for (size_t i = 0; i < si.count; ++i) {
        if (si.props[i].pid == pid && si.props[i].type == RP_VT_LPSTR) {
            char *s = zalloc(p, si.props[i].str_len + 1, 1);
            if (s) memcpy(s, si.props[i].str, si.props[i].str_len);
            return s ? s : "";
        }
    }
    return "";
}

// ---- the packages' files --------------------------------------------------------------------

typedef struct {
    rp_cab_file_t *v;
    size_t         count;
} files_t;

// Every file of the package's embedded cabinets.
static proven_err_t load_files(pctx_t *p, const rp_mst_side_t *side, files_t *out) {
    *out = (files_t){ 0 };
    const rp_msi_wtable_t *m = table(side->db, "Media");
    int cab = col(m, "Cabinet");
    if (m == NULL || cab < 0) return PROVEN_OK;
    for (size_t r = 0; r < m->row_count; ++r) {
        const rp_msi_cell_t *c = &m->cells[r * m->column_count + (size_t)cab];
        if (c->kind != RP_MSI_STR || c->len == 0) continue;
        if (c->bytes[0] != '#') return refuse(p, (const char *)c->bytes, c->len, "is an external cabinet; a patch is made from packages whose cabinets are embedded");
        char name[80];
        snprintf(name, sizeof name, "%.*s", (int)(c->len - 1), (const char *)c->bytes + 1);
        uint16_t packed[32];
        size_t pl;
        uint32_t id;
        if (rp_msi_stream_name(name, false, packed, &pl) != PROVEN_OK || rp_cfb_find(side->cfb, 0, packed, pl, &id) != PROVEN_OK) {
            return refuse(p, name, strlen(name), "is a cabinet the package does not hold");
        }
        size_t n = (size_t)side->cfb->entries[id].size;
        uint8_t *data = zalloc(p, n, 1), *arena = NULL;
        rp_cab_file_t *files = NULL;
        size_t count = 0;
        if (data == NULL) return PROVEN_ERR_NOMEM;
        proven_err_t err = rp_cfb_read(side->cfb, id, data, n);
        if (err == PROVEN_OK) err = rp_cab_read(p->alloc, data, n, p->limits, &files, &count, &arena);
        if (err != PROVEN_OK) return err;
        own(p, files);
        own(p, arena);
        rp_cab_file_t *all = zalloc(p, out->count + count, sizeof *all);
        if (all == NULL) return PROVEN_ERR_NOMEM;
        if (out->count) memcpy(all, out->v, out->count * sizeof *all);
        if (count) memcpy(all + out->count, files, count * sizeof *all);
        out->v = all;
        out->count += count;
    }
    return PROVEN_OK;
}

static const rp_cab_file_t *file_bytes(const files_t *f, const rp_msi_cell_t *key) {
    for (size_t i = 0; i < f->count; ++i) {
        if (key->kind == RP_MSI_STR && strlen(f->v[i].name) == key->len && memcmp(f->v[i].name, key->bytes, key->len) == 0) {
            return &f->v[i];
        }
    }
    return NULL;
}

// ---- tables built for the transforms ---------------------------------------------------------

static const rp_msi_wcolumn_t patch_cols[] = {
    { "File_", 0x2D48 }, { "Sequence", 0x2502 }, { "PatchSize", 0x0104 }, { "Attributes", 0x0502 },
    { "Header", 0x1900 }, { "StreamRef_", 0x1D48 },
};
static const rp_msi_wcolumn_t package_cols[] = { { "PatchId", 0x2D26 }, { "Media_", 0x0502 } };
static const rp_msi_wcolumn_t headers_cols[] = { { "StreamRef", 0x2D26 }, { "Header", 0x0900 } };
static const rp_msi_wcolumn_t metadata_cols[] = { { "Company", 0x3D00 }, { "Property", 0x2D00 }, { "Value", 0x1D00 } };
static const rp_msi_wcolumn_t sequence_cols[] = {
    { "PatchFamily", 0x2D00 }, { "ProductCode", 0x3D26 }, { "Sequence", 0x0D00 }, { "Attributes", 0x1502 },
};

// A copy of t's cells with room for `extra` rows more.
static rp_msi_cell_t *grow(pctx_t *p, const rp_msi_wtable_t *t, size_t extra) {
    size_t n = t->row_count * t->column_count;
    rp_msi_cell_t *c = zalloc(p, n + extra * t->column_count, sizeof *c);
    if (c && n) memcpy(c, t->cells, n * sizeof *c);
    return c;
}

// Appends a row given by column name; the other cells stay null.
static void put_row(rp_msi_wtable_t *t, rp_msi_cell_t *cells, const char *const *names, const rp_msi_cell_t *values, size_t n) {
    rp_msi_cell_t *row = &cells[t->row_count * t->column_count];
    for (size_t c = 0; c < t->column_count; ++c) row[c] = (rp_msi_cell_t){ .kind = RP_MSI_NULL };
    for (size_t k = 0; k < n; ++k) {
        int c = col(t, names[k]);
        if (c >= 0) row[c] = values[k];
    }
    t->cells = cells;
    t->row_count++;
}

// ---- composing the file ----------------------------------------------------------------------

typedef struct {
    rp_cfb_stream_t v[512];
    size_t          count;
} list_t;

// The streams of a compound file's root, under `parent` (0: the root) in `list`.
static proven_err_t take_streams(pctx_t *p, const rp_cfb_t *cfb, size_t parent, list_t *list) {
    uint32_t ids[256];
    size_t n = 0;
    proven_err_t err = rp_cfb_children(cfb, 0, ids, sizeof ids / sizeof ids[0], &n);
    for (size_t i = 0; err == PROVEN_OK && i < n; ++i) {
        const rp_cfb_entry_t *e = &cfb->entries[ids[i]];
        if (e->type != 2) continue;
        if (list->count == sizeof list->v / sizeof list->v[0]) return PROVEN_ERR_OUT_OF_BOUNDS;
        uint8_t *data = zalloc(p, (size_t)e->size, 1);
        if (data == NULL) return PROVEN_ERR_NOMEM;
        err = rp_cfb_read(cfb, ids[i], data, (size_t)e->size);
        list->v[list->count++] = (rp_cfb_stream_t){ .name = e->name, .name_len = e->name_len, .data = data, .size = (size_t)e->size,
                                                    .parent = parent };
    }
    return err;
}

static proven_err_t add_storage(list_t *list, const uint16_t *name, size_t len, const uint8_t *clsid, size_t *index) {
    if (list->count == sizeof list->v / sizeof list->v[0]) return PROVEN_ERR_OUT_OF_BOUNDS;
    list->v[list->count] = (rp_cfb_stream_t){ .name = name, .name_len = len, .storage = true, .clsid = clsid };
    *index = ++list->count;     // children name it as parent: index + 1
    return PROVEN_OK;
}

static bool ascii(const char *s) {
    for (; *s; ++s) {
        if ((unsigned char)*s >= 0x80) return false;
    }
    return true;
}

proven_err_t rp_msp_write(proven_allocator_t alloc, const rp_mst_side_t *base, const rp_mst_side_t *target, const rp_msp_opts_t *o,
                          const rp_limits_t *limits, uint8_t **out, size_t *len, const char **why, char *tname, size_t tname_cap) {
    if (base == NULL || target == NULL || o == NULL || limits == NULL || out == NULL || len == NULL) return PROVEN_ERR_INVALID_ARG;
    pctx_t *p = rp_mem_alloc(alloc, 1, sizeof *p);     // MAX_OWN pointers: not on the stack
    if (p == NULL) return PROVEN_ERR_NOMEM;
    *p = (pctx_t){ .alloc = alloc, .limits = limits, .table = tname, .table_cap = tname_cap };
    if (tname && tname_cap) tname[0] = '\0';
    const rp_msi_wdb_t *A = base->db, *B = target->db;
    proven_err_t err = PROVEN_OK;
    uint8_t *mst1 = NULL, *mst2 = NULL, *db = NULL, *cab = NULL, *sum = NULL;
    size_t mst1_len = 0, mst2_len = 0, db_len = 0, cab_len = 0, sum_len = 0;
    rp_cfb_t c_db = { 0 }, c_1 = { 0 }, c_2 = { 0 };
    int opened = 0;
    list_t *list = NULL;

    // What a patch can carry: the same product, a new version in the third field at most.
    char *pc_a = property(p, A, "ProductCode"), *pc_b = property(p, B, "ProductCode");
    char *uc_a = property(p, A, "UpgradeCode"), *uc_b = property(p, B, "UpgradeCode");
    char *ver_a = property(p, A, "ProductVersion"), *ver_b = property(p, B, "ProductVersion");
    char *name = property(p, B, "ProductName"), *maker = property(p, B, "Manufacturer");
    if (!pc_a || !pc_b || !uc_a || !uc_b || !ver_a || !ver_b || !name || !maker) err = PROVEN_ERR_NOMEM;
    long va[4], vb[4];
    if (err == PROVEN_OK) {
        version(ver_a, va);
        version(ver_b, vb);
        if (strcmp(pc_a, pc_b) != 0) err = refuse(p, "ProductCode", 11, "differs: that is a major upgrade, which a patch does not carry");
        else if (strcmp(uc_a, uc_b) != 0) err = refuse(p, "UpgradeCode", 11, "differs between the two packages");
        else if (va[0] != vb[0] || va[1] != vb[1]) err = refuse(p, "ProductVersion", 14, "changes in its first or second field; a patch changes the third at most");
        else if (vb[2] < va[2] || (vb[2] == va[2] && vb[3] < va[3])) err = refuse(p, "ProductVersion", 14, "is lower in the target than in the base");
        else if (A->codepage != B->codepage) err = refuse(p, "", 0, "the two packages have different code pages");
    }

    // Components and files may change and be added, not removed.
    const rp_msi_wtable_t *ca = table(A, "Component"), *cb = table(B, "Component");
    int cid = col(cb, "ComponentId");
    for (size_t r = 0; err == PROVEN_OK && ca && r < ca->row_count; ++r) {
        const rp_msi_cell_t *ra = &ca->cells[r * ca->column_count], *rb = cb ? row_by_key(cb, &ra[0]) : NULL;
        if (rb == NULL) err = refuse(p, (const char *)ra[0].bytes, ra[0].len, "is a component the target no longer has; a patch cannot remove components");
        else if (cid >= 0 && !same_str(&ra[cid], &rb[cid])) err = refuse(p, (const char *)ra[0].bytes, ra[0].len, "is a component whose GUID changed");
    }
    const rp_msi_wtable_t *fa = table(A, "File"), *fb = table(B, "File");
    int fseq = col(fb, "Sequence"), fattr = col(fb, "Attributes");
    if (err == PROVEN_OK && fb && (fseq < 0 || fattr < 0 || (fa && col(fa, "Sequence") != fseq))) err = refuse(p, "File", 4, "has columns rubrapack does not know");
    for (size_t r = 0; err == PROVEN_OK && fa && r < fa->row_count; ++r) {
        const rp_msi_cell_t *ra = &fa->cells[r * fa->column_count];
        if (!fb || row_by_key(fb, &ra[0]) == NULL) err = refuse(p, (const char *)ra[0].bytes, ra[0].len, "is a file the target no longer has; a patch cannot remove files");
    }

    // The files the patch carries: new ones, and those whose bytes changed.
    files_t bytes_a = { 0 }, bytes_b = { 0 };
    size_t *pick = NULL, npick = 0;
    if (err == PROVEN_OK && fb && fb->row_count) {
        err = load_files(p, base, &bytes_a);
        if (err == PROVEN_OK) err = load_files(p, target, &bytes_b);
        pick = zalloc(p, fb->row_count, sizeof *pick);
        if (err == PROVEN_OK && pick == NULL) err = PROVEN_ERR_NOMEM;
        for (size_t r = 0; err == PROVEN_OK && r < fb->row_count; ++r) {
            const rp_msi_cell_t *rb = &fb->cells[r * fb->column_count];
            const rp_cab_file_t *nb = file_bytes(&bytes_b, &rb[0]);
            if (nb == NULL) {
                err = refuse(p, (const char *)rb[0].bytes, rb[0].len, "is a file that is not in an embedded cabinet of the target");
                break;
            }
            const rp_msi_cell_t *ra = fa ? row_by_key(fa, &rb[0]) : NULL;
            const rp_cab_file_t *na = ra ? file_bytes(&bytes_a, &rb[0]) : NULL;
            if (ra && na == NULL) {
                err = refuse(p, (const char *)rb[0].bytes, rb[0].len, "is a file that is not in an embedded cabinet of the base");
                break;
            }
            if (ra == NULL || na->size != nb->size || memcmp(na->data, nb->data, nb->size) != 0) {
                size_t k = npick++;      // in the target's Sequence order
                while (k > 0 && fb->cells[pick[k - 1] * fb->column_count + (size_t)fseq].i > rb[fseq].i) {
                    pick[k] = pick[k - 1];
                    --k;
                }
                pick[k] = r;
            }
        }
    }

    // RP1's target: the target, but existing files keep their Sequence and the Media table stays.
    rp_msi_wtable_t *tp = zalloc(p, B->table_count, sizeof *tp);
    if (err == PROVEN_OK && tp == NULL) err = PROVEN_ERR_NOMEM;
    const rp_msi_wtable_t *ma = table(A, "Media");
    int32_t max_seq = 0, max_disk = 0;
    for (size_t r = 0; ma && r < ma->row_count; ++r) {
        const rp_msi_cell_t *row = &ma->cells[r * ma->column_count];
        int ls = col(ma, "LastSequence");
        if (row[0].kind == RP_MSI_INT && row[0].i > max_disk) max_disk = row[0].i;
        if (ls >= 0 && row[ls].kind == RP_MSI_INT && row[ls].i > max_seq) max_seq = row[ls].i;
    }
    for (size_t t = 0; err == PROVEN_OK && t < B->table_count; ++t) {
        tp[t] = B->tables[t];
        if (&B->tables[t] == fb) {
            rp_msi_cell_t *cells = grow(p, fb, 0);
            if (cells == NULL) err = PROVEN_ERR_NOMEM;
            for (size_t r = 0; err == PROVEN_OK && r < fb->row_count; ++r) {
                rp_msi_cell_t *row = &cells[r * fb->column_count];
                const rp_msi_cell_t *ra = fa ? row_by_key(fa, &row[0]) : NULL;
                if (ra) row[fseq] = ra[fseq];
                if (row[fseq].kind == RP_MSI_INT && row[fseq].i > max_seq) max_seq = row[fseq].i;
            }
            tp[t].cells = cells;
        } else if (strcmp(B->tables[t].name, "Media") == 0 && ma) {
            tp[t] = *ma;
        }
    }
    rp_msi_wdb_t tpdb = *B;
    tpdb.tables = tp;
    rp_mst_side_t tps = { &tpdb, target->cfb };

    char patch_code[39];
    if (o->patch_code) {
        snprintf(patch_code, sizeof patch_code, "%s", o->patch_code);
    } else {
        const char *fields[2] = { sum_str(p, A, 9), sum_str(p, B, 9) };
        rp_uuid_derive("patch", fields, 2, patch_code);
    }
    const char *new_package = sum_str(p, B, 9), *new_subject = sum_str(p, B, 3), *new_comments = sum_str(p, B, 6);
    int32_t start = (max_seq / 1000 + 1) * 1000, disk = max_disk + 1;
    static const char cab_name[] = "RP_PATCH_CAB";

    // #RP1's target: RP1's plus what makes it a patch.
    size_t extra_tables = 3;       // Windows wants PatchPackage even in a patch that carries no file
    rp_msi_wtable_t *tpp = zalloc(p, B->table_count + extra_tables, sizeof *tpp);
    if (err == PROVEN_OK && tpp == NULL) err = PROVEN_ERR_NOMEM;
    for (size_t t = 0; err == PROVEN_OK && t < B->table_count; ++t) {
        rp_msi_wtable_t *x = &tpp[t];
        *x = tp[t];
        if ((strcmp(x->name, "Patch") == 0 || strcmp(x->name, "PatchPackage") == 0 || strcmp(x->name, "MsiPatchHeaders") == 0)) {
            err = refuse(p, x->name, strlen(x->name), "is a table the target already has; rubrapack does not patch a package that holds patch tables");
        } else if (npick && strcmp(x->name, "File") == 0) {
            rp_msi_cell_t *cells = grow(p, x, 0);
            if (cells == NULL) err = PROVEN_ERR_NOMEM;
            for (size_t k = 0; err == PROVEN_OK && k < npick; ++k) {
                rp_msi_cell_t *row = &cells[pick[k] * x->column_count];
                row[fattr] = int_cell((row[fattr].kind == RP_MSI_INT ? row[fattr].i : 0) | PATCH_ADDED);
                row[fseq] = int_cell(start + (int32_t)k);
            }
            x->cells = cells;
        } else if (strcmp(x->name, "Media") == 0) {
            rp_msi_cell_t *cells = grow(p, x, 1);
            char *hash_cab = zalloc(p, sizeof cab_name + 1, 1);
            if (cells == NULL || hash_cab == NULL) {
                err = PROVEN_ERR_NOMEM;
                break;
            }
            snprintf(hash_cab, sizeof cab_name + 1, "#%s", cab_name);
            static const char *const names[] = { "DiskId", "LastSequence", "Cabinet", "Source" };
            rp_msi_cell_t values[] = { int_cell(disk), int_cell(start + (int32_t)npick), str_cell(npick ? hash_cab : NULL),
                                       str_cell("RPPATCHSRC") };
            put_row(x, cells, names, values, 4);
        } else if (strcmp(x->name, "Property") == 0) {
            rp_msi_cell_t *cells = grow(p, x, 3);
            if (cells == NULL) {
                err = PROVEN_ERR_NOMEM;
                break;
            }
            static const char *const names[] = { "Property", "Value" };
            rp_msi_cell_t v1[] = { str_cell("PATCHNEWPACKAGECODE"), str_cell(new_package) };
            rp_msi_cell_t v2[] = { str_cell("PATCHNEWSUMMARYSUBJECT"), str_cell(new_subject) };
            rp_msi_cell_t v3[] = { str_cell("PATCHNEWSUMMARYCOMMENTS"), str_cell(new_comments) };
            if (*new_package) put_row(x, cells, names, v1, 2);
            if (*new_subject) put_row(x, cells, names, v2, 2);
            if (*new_comments) put_row(x, cells, names, v3, 2);
        } else if ((strcmp(x->name, "InstallExecuteSequence") == 0 || strcmp(x->name, "AdminExecuteSequence") == 0)) {
            rp_msi_cell_t key = str_cell("PatchFiles");
            if (row_by_key(x, &key) == NULL) {
                rp_msi_cell_t *cells = grow(p, x, 1);
                if (cells == NULL) {
                    err = PROVEN_ERR_NOMEM;
                    break;
                }
                static const char *const names[] = { "Action", "Sequence" };
                rp_msi_cell_t values[] = { key, int_cell(PATCH_FILES_SEQ) };
                put_row(x, cells, names, values, 2);
            }
        }
    }
    size_t ntpp = B->table_count;
    if (err == PROVEN_OK) {
        rp_msi_cell_t *pkg = zalloc(p, 2, sizeof *pkg);
        if (pkg == NULL) err = PROVEN_ERR_NOMEM;
        else {
            pkg[0] = str_cell(patch_code);
            pkg[1] = int_cell(disk);
            tpp[ntpp++] = (rp_msi_wtable_t){ .name = "Patch", .columns = patch_cols, .column_count = 6 };
            tpp[ntpp++] = (rp_msi_wtable_t){ .name = "PatchPackage", .columns = package_cols, .column_count = 2, .cells = pkg, .row_count = 1 };
            tpp[ntpp++] = (rp_msi_wtable_t){ .name = "MsiPatchHeaders", .columns = headers_cols, .column_count = 2 };
        }
    }
    rp_msi_wdb_t tppdb = tpdb;
    tppdb.tables = tpp;
    tppdb.table_count = ntpp;
    rp_mst_side_t tpps = { &tppdb, target->cfb };

    // The two transforms, with the checks and ignored errors MsiMsp.exe writes.
    const char *mwhy = NULL;
    if (err == PROVEN_OK) {
        rp_mst_opts_t t1 = { .summary_flags = 0x0922u << 16 | 0x0017u, .files = true };
        err = rp_mst_write(alloc, base, &tps, &t1, limits, &mst1, &mst1_len, &mwhy, tname, tname_cap);
        if (err == PROVEN_ERR_UNSUPPORTED) p->why = mwhy;
    }
    if (err == PROVEN_OK) {
        rp_mst_opts_t t2 = { .summary_flags = 0x0927u << 16 | 0x0017u, .files = true };
        err = rp_mst_write(alloc, &tps, &tpps, &t2, limits, &mst2, &mst2_len, &mwhy, tname, tname_cap);
        if (err == PROVEN_ERR_UNSUPPORTED) p->why = mwhy;
    }

    // The cabinet: the carried files whole, named by their File keys, in their new order.
    if (err == PROVEN_OK && npick) {
        rp_cab_file_t *cf = zalloc(p, npick, sizeof *cf);
        char **keys = zalloc(p, npick, sizeof *keys);
        if (cf == NULL || keys == NULL) err = PROVEN_ERR_NOMEM;
        for (size_t k = 0; err == PROVEN_OK && k < npick; ++k) {
            const rp_msi_cell_t *rb = &fb->cells[pick[k] * fb->column_count];
            const rp_cab_file_t *nb = file_bytes(&bytes_b, &rb[0]);
            keys[k] = cstr(p, &rb[0]);
            if (keys[k] == NULL) err = PROVEN_ERR_NOMEM;
            cf[k] = (rp_cab_file_t){ keys[k], nb->data, nb->size };
        }
        if (err == PROVEN_OK) err = rp_cab_write(alloc, cf, npick, 6, limits, &cab, &cab_len);
    }

    // The root database: MsiPatchMetadata and MsiPatchSequence.
    char display[512], seq[64], *url = NULL;
    snprintf(display, sizeof display, "%s %s", name ? name : "", ver_b ? ver_b : "");
    snprintf(seq, sizeof seq, "%ld.%ld.%ld.%ld", vb[0], vb[1], vb[2], vb[3]);
    if (err == PROVEN_OK) {
        url = property(p, B, "ARPHELPLINK");
        if (url && !*url) url = property(p, B, "ARPURLINFOABOUT");
        if (url == NULL) err = PROVEN_ERR_NOMEM;
    }
    if (err == PROVEN_OK) {
        const char *meta[][2] = {
            { "AllowRemoval", o->no_removal ? "0" : "1" }, { "Classification", "Update" }, { "Description", display },
            { "DisplayName", display }, { "ManufacturerName", maker }, { "MoreInfoURL", url }, { "TargetProductName", name },
        };
        rp_msi_cell_t *mc = zalloc(p, 3 * 7, sizeof *mc), sc[4];
        size_t nm = 0;
        bool all_ascii = ascii(display) && ascii(maker) && ascii(url) && ascii(name) && (!o->family || ascii(o->family));
        for (size_t i = 0; mc && i < 7; ++i) {
            if (!*meta[i][1]) continue;
            mc[3 * nm] = (rp_msi_cell_t){ .kind = RP_MSI_NULL };
            mc[3 * nm + 1] = str_cell(meta[i][0]);
            mc[3 * nm + 2] = str_cell(meta[i][1]);
            ++nm;
        }
        sc[0] = str_cell(o->family ? o->family : name);
        sc[1] = (rp_msi_cell_t){ .kind = RP_MSI_NULL };
        sc[2] = str_cell(seq);
        sc[3] = int_cell(0);
        rp_msi_wtable_t rt[2] = {
            { .name = "MsiPatchMetadata", .columns = metadata_cols, .column_count = 3, .cells = mc, .row_count = nm },
            { .name = "MsiPatchSequence", .columns = sequence_cols, .column_count = 4, .cells = sc, .row_count = 1 },
        };
        rp_msi_wdb_t rdb = { .codepage = all_ascii ? 0 : B->codepage, .tables = rt, .table_count = 2 };
        err = mc ? rp_msi_write(alloc, &rdb, 9, limits, &db, &db_len) : PROVEN_ERR_NOMEM;
    }

    // Summary information: the source list, the targets, the transforms in order, the patch code,
    // and Windows Installer 3.0 (MsiPatchSequence) at least.
    static const char storages[] = ":RP1;:#RP1";
    if (err == PROVEN_OK) {
        const char *src = o->source_list ? o->source_list : "";
        rp_suminfo_t si = { .count = 5 };
        si.props[0] = (rp_suminfo_prop_t){ .pid = 5, .type = RP_VT_LPSTR, .str = (const uint8_t *)src, .str_len = strlen(src) };
        si.props[1] = (rp_suminfo_prop_t){ .pid = 7, .type = RP_VT_LPSTR, .str = (const uint8_t *)pc_b, .str_len = strlen(pc_b) };
        si.props[2] = (rp_suminfo_prop_t){ .pid = 8, .type = RP_VT_LPSTR, .str = (const uint8_t *)storages, .str_len = sizeof storages - 1 };
        si.props[3] = (rp_suminfo_prop_t){ .pid = 9, .type = RP_VT_LPSTR, .str = (const uint8_t *)patch_code, .str_len = strlen(patch_code) };
        si.props[4] = (rp_suminfo_prop_t){ .pid = 15, .type = RP_VT_I4, .i = 4 };
        err = rp_suminfo_write(&si, false, alloc, &sum, &sum_len);
    }

    // Composed: the root database's streams, the cabinet, the summary, then RP1 and #RP1.
    static const uint8_t msp_clsid[16] = { 0x86, 0x10, 0x0C, 0, 0, 0, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46 };
    static const uint8_t mst_clsid[16] = { 0x82, 0x10, 0x0C, 0, 0, 0, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46 };
    static const uint16_t s1[] = { 'R', 'P', '1' }, s2[] = { '#', 'R', 'P', '1' };
    static const uint16_t sname[] = { 5, 'S', 'u', 'm', 'm', 'a', 'r', 'y', 'I', 'n', 'f', 'o', 'r', 'm', 'a', 't', 'i', 'o', 'n' };
    uint16_t cab_packed[32];
    size_t cab_pl = 0;
    if (err == PROVEN_OK) err = rp_cfb_open(&c_db, alloc, db, db_len, limits, &mwhy);
    if (err == PROVEN_OK) ++opened, err = rp_cfb_open(&c_1, alloc, mst1, mst1_len, limits, &mwhy);
    if (err == PROVEN_OK) ++opened, err = rp_cfb_open(&c_2, alloc, mst2, mst2_len, limits, &mwhy);
    if (err == PROVEN_OK) ++opened;
    list = err == PROVEN_OK ? zalloc(p, 1, sizeof *list) : NULL;
    if (err == PROVEN_OK && list == NULL) err = PROVEN_ERR_NOMEM;
    if (err == PROVEN_OK) err = take_streams(p, &c_db, 0, list);
    if (err == PROVEN_OK && npick) {
        err = rp_msi_stream_name(cab_name, false, cab_packed, &cab_pl);
        if (err == PROVEN_OK) list->v[list->count++] = (rp_cfb_stream_t){ .name = cab_packed, .name_len = cab_pl, .data = cab, .size = cab_len };
    }
    if (err == PROVEN_OK) list->v[list->count++] = (rp_cfb_stream_t){ .name = sname, .name_len = 19, .data = sum, .size = sum_len };
    size_t st1 = 0, st2 = 0;
    if (err == PROVEN_OK) err = add_storage(list, s1, 3, mst_clsid, &st1);
    if (err == PROVEN_OK) err = take_streams(p, &c_1, st1, list);
    if (err == PROVEN_OK) err = add_storage(list, s2, 4, mst_clsid, &st2);
    if (err == PROVEN_OK) err = take_streams(p, &c_2, st2, list);
    if (err == PROVEN_OK) err = rp_cfb_write(alloc, 9, msp_clsid, list->v, list->count, limits, out, len);

    if (why) *why = p->why;
    if (opened >= 3) rp_cfb_close(&c_2);
    if (opened >= 2) rp_cfb_close(&c_1);
    if (opened >= 1) rp_cfb_close(&c_db);
    rp_mem_free(alloc, mst1);
    rp_mem_free(alloc, mst2);
    rp_mem_free(alloc, db);
    rp_mem_free(alloc, cab);
    rp_mem_free(alloc, sum);
    for (size_t i = 0; i < p->nown; ++i) rp_mem_free(alloc, p->own[i]);
    rp_mem_free(alloc, p);
    return err;
}
