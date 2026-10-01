// src/cli/extract.c - `rubrapack extract <file.msi|file.cab> -d <new dir>` (RFC-0006 2, L2;
// RFC-0001 14.3).
//
// An MSI is laid out as it installs: the Directory tree by long target names (a standard folder
// such as ProgramFiles64Folder under the root keeps its key as the folder name), each file under
// its component's directory by its long name. Files come from the cabinets of the Media table
// (embedded "#name" streams or files next to the package) or, when uncompressed, from the source
// tree next to the package. A CAB is laid out by the names inside it.
//
// Nothing is written until the whole plan passes: every name is checked (no "..", absolute
// paths, drives, ':' streams, reserved device names, trailing dots or spaces, control
// characters), no two paths differ only in ASCII case, sizes and MsiFileHash agree, and the entry
// and byte limits hold. Then the target (missing or empty) is filled with new directories and
// new files only - an existing name is never opened, followed or replaced.

#include "rubrapack/cab.h"
#include "rubrapack/chain.h"
#include "rubrapack/cfb.h"
#include "rubrapack/diag.h"
#include "rubrapack/inspect.h"
#include "rubrapack/md5.h"
#include "rubrapack/mem.h"
#include "rubrapack/msi.h"
#include "rubrapack/msix.h"
#include "rubrapack/pal.h"
#include "rubrapack/suminfo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "proven/heap.h"

enum { MAX_INPUT = 1u << 30, MAX_NAME = 255 };

typedef struct {
    char          *rel;         // relative path, '/' separated
    char          *fold;        // ASCII lower case, for collisions
    bool           dir;
    const uint8_t *data;
    size_t         size;
} item_t;

typedef struct {
    proven_allocator_t alloc;
    rp_limits_t        limits;      // what the plan may write (--limit-entries, --limit-bytes)
    rp_limits_t        read;        // for the readers: the defaults, raised to the plan's limits
    item_t            *items;
    size_t             count, cap;
    void             **owned;   // buffers to free at the end (cabinet arenas, loose files, ...)
    size_t             nowned, capowned;
    bool               nomem;
    const char        *path;    // the input, for messages
} plan_t;

static void own(plan_t *p, void *ptr) {
    if (ptr == NULL) return;
    if (p->nowned == p->capowned) {
        size_t cap = p->capowned ? p->capowned * 2 : 32;
        void **n = rp_mem_alloc(p->alloc, cap, sizeof *n);
        if (n == NULL) {
            p->nomem = true;
            rp_mem_free(p->alloc, ptr);
            return;
        }
        if (p->nowned) memcpy(n, p->owned, p->nowned * sizeof *n);
        rp_mem_free(p->alloc, p->owned);
        p->owned = n;
        p->capowned = cap;
    }
    p->owned[p->nowned++] = ptr;
}

static char *dup_n(plan_t *p, const char *s, size_t n) {
    char *d = rp_mem_alloc(p->alloc, n + 1, 1);
    if (d == NULL) {
        p->nomem = true;
        return NULL;
    }
    memcpy(d, s, n);
    d[n] = '\0';
    own(p, d);
    return d;
}

static char *join(plan_t *p, const char *a, const char *b) {
    if (a == NULL || b == NULL) return NULL;
    if (a[0] == '\0') return dup_n(p, b, strlen(b));
    size_t na = strlen(a), nb = strlen(b);
    char *d = rp_mem_alloc(p->alloc, na + nb + 2, 1);
    if (d == NULL) {
        p->nomem = true;
        return NULL;
    }
    memcpy(d, a, na);
    d[na] = '/';
    memcpy(d + na + 1, b, nb + 1);
    own(p, d);
    return d;
}

// Why one path component is refused, or NULL.
static const char *name_problem(const char *s, size_t n) {
    if (n == 0) return "an empty name";
    if (n > MAX_NAME) return "a name longer than 255 bytes";
    if ((n == 1 && s[0] == '.') || (n == 2 && s[0] == '.' && s[1] == '.')) return "'.' or '..'";
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c == 0x7F) return "a control character";
        if (strchr("/\\:*?\"<>|", c)) return "a character Windows does not allow in a name (/ \\ : * ? \" < > |)";
    }
    if (s[n - 1] == ' ' || s[n - 1] == '.') return "a name ending with a space or a dot";
    // Reserved device names, with or without an extension.
    size_t base = 0;
    while (base < n && s[base] != '.') ++base;
    static const char *const dev[] = { "CON", "PRN", "AUX", "NUL", NULL };
    char up[5] = { 0 };
    if (base == 3 || base == 4) {
        for (size_t i = 0; i < base; ++i) up[i] = (char)(s[i] >= 'a' && s[i] <= 'z' ? s[i] - 32 : s[i]);
        for (size_t i = 0; base == 3 && dev[i]; ++i) {
            if (memcmp(up, dev[i], 3) == 0) return "a reserved device name";
        }
        if (base == 4 && (memcmp(up, "COM", 3) == 0 || memcmp(up, "LPT", 3) == 0) && up[3] >= '1' && up[3] <= '9') {
            return "a reserved device name";
        }
    }
    return NULL;
}

// Checks every component of a '/' path.
static bool path_ok(plan_t *p, const char *rel, const char *what) {
    for (const char *s = rel;;) {
        const char *e = strchr(s, '/');
        size_t n = e ? (size_t)(e - s) : strlen(s);
        const char *why = name_problem(s, n);
        if (why) {
            rp_diag_error(RP_DIAG_BAD_PACKAGE, "'%s': %s '%s' has %s; nothing was extracted", p->path, what, rel, why);
            return false;
        }
        if (e == NULL) return true;
        s = e + 1;
    }
}

static bool add_item(plan_t *p, char *rel, bool dir, const uint8_t *data, size_t size) {
    if (rel == NULL) return false;
    for (size_t i = 0; i < p->count; ++i) {         // a directory is added once
        if (dir && p->items[i].dir && strcmp(p->items[i].rel, rel) == 0) return true;
    }
    if (p->count == p->cap) {
        size_t cap = p->cap ? p->cap * 2 : 64;
        item_t *n = rp_mem_alloc(p->alloc, cap, sizeof *n);
        if (n == NULL) {
            p->nomem = true;
            return false;
        }
        if (p->count) memcpy(n, p->items, p->count * sizeof *n);
        rp_mem_free(p->alloc, p->items);
        p->items = n;
        p->cap = cap;
    }
    char *fold = dup_n(p, rel, strlen(rel));
    if (fold == NULL) return false;
    for (char *c = fold; *c; ++c) {
        if (*c >= 'A' && *c <= 'Z') *c = (char)(*c + 32);
    }
    p->items[p->count++] = (item_t){ rel, fold, dir, data, size };
    return true;
}

// A file and every folder above it.
static bool add_file(plan_t *p, char *rel, const uint8_t *data, size_t size) {
    for (char *s = strchr(rel, '/'); s; s = strchr(s + 1, '/')) {
        if (!add_item(p, dup_n(p, rel, (size_t)(s - rel)), true, NULL, 0)) return false;
    }
    return add_item(p, rel, false, data, size);
}

// By the folded path, then by the path as written, then folders first: a total order.
static int cmp_fold(const void *a, const void *b) {
    const item_t *x = a, *y = b;
    int c = strcmp(x->fold, y->fold);
    if (c == 0) c = strcmp(x->rel, y->rel);
    return c ? c : (int)y->dir - (int)x->dir;
}

// ---- writing ---------------------------------------------------------------------------------

static int write_plan(plan_t *p, const char *dest) {
    // Paths that differ only in case would be one file on Windows.
    rp_sort(p->items, p->count, sizeof *p->items, cmp_fold);
    for (size_t i = 1; i < p->count; ++i) {
        if (strcmp(p->items[i - 1].fold, p->items[i].fold) == 0 && !(p->items[i - 1].dir && p->items[i].dir)) {
            rp_diag_error(RP_DIAG_BAD_PACKAGE, "'%s': '%s' and '%s' would be the same path on Windows; nothing was extracted",
                          p->path, p->items[i - 1].rel, p->items[i].rel);
            return RP_EXIT_IO;
        }
    }
    if (p->count > p->limits.max_entries) {
        rp_diag_error(RP_DIAG_BAD_PACKAGE, "'%s': %zu entries, more than the limit %llu (--limit-entries); nothing was extracted",
                      p->path, p->count, (unsigned long long)p->limits.max_entries);
        return RP_EXIT_IO;
    }
    rp_budget_t bytes = rp_budget(p->limits.max_output);
    for (size_t i = 0; i < p->count; ++i) {
        if (rp_budget_charge(&bytes, p->items[i].size) != PROVEN_OK) {
            rp_diag_error(RP_DIAG_BAD_PACKAGE, "'%s': more than %llu bytes to write (--limit-bytes); nothing was extracted",
                          p->path, (unsigned long long)p->limits.max_output);
            return RP_EXIT_IO;
        }
    }
    uint64_t size = 0;
    rp_fskind_t k = rp_pal_stat(p->alloc, dest, &size);
    if (k == RP_FS_NONE) {
        if (rp_pal_mkdir_new(p->alloc, dest) != PROVEN_OK) {
            rp_diag_error(RP_DIAG_OUTPUT, "cannot create '%s'", dest);
            return RP_EXIT_IO;
        }
    } else {
        char **names = NULL;
        size_t n = 0;
        bool empty = k == RP_FS_DIR && rp_pal_list_dir(p->alloc, dest, &names, &n) == PROVEN_OK && n == 0;
        for (size_t i = 0; i < n; ++i) rp_mem_free(p->alloc, names[i]);
        rp_mem_free(p->alloc, names);
        if (!empty) {
            rp_diag_error(RP_DIAG_OUTPUT, "'%s' exists and is not an empty directory; extract into a new one", dest);
            return RP_EXIT_IO;
        }
    }
    // Folders first (a parent sorts before its children), then the files.
    size_t files = 0;
    uint64_t total = 0;
    for (int pass = 0; pass < 2; ++pass) {
        for (size_t i = 0; i < p->count; ++i) {
            const item_t *it = &p->items[i];
            if (it->dir != (pass == 0)) continue;
            char *full = join(p, dest, it->rel);
            if (full == NULL) return RP_EXIT_IO;
            proven_err_t err = it->dir ? rp_pal_mkdir_new(p->alloc, full) : rp_pal_write_file_new(p->alloc, full, it->data, it->size);
            if (err != PROVEN_OK) {
                rp_diag_error(RP_DIAG_OUTPUT, "cannot create '%s'%s", full, err == PROVEN_ERR_BUSY ? " (the name is taken)" : "");
                return RP_EXIT_IO;
            }
            if (!it->dir) {
                ++files;
                total += it->size;
            }
        }
    }
    char line[1200];
    snprintf(line, sizeof line, "%s: %zu files, %llu bytes\n", dest, files, (unsigned long long)total);
    return rp_pal_puts(RP_OUT_STDOUT, line) == PROVEN_OK ? RP_EXIT_OK : RP_EXIT_IO;
}

// ---- CAB -------------------------------------------------------------------------------------

static int plan_cab(plan_t *p, const uint8_t *data, size_t len) {
    rp_cab_file_t *files = NULL;
    size_t n = 0;
    uint8_t *arena = NULL;
    if (rp_cab_read(p->alloc, data, len, &p->read, &files, &n, &arena) != PROVEN_OK) {
        rp_diag_error(RP_DIAG_BAD_PACKAGE, "'%s' is not a cabinet rubrapack can read (stored or MSZIP)", p->path);
        return RP_EXIT_IO;
    }
    own(p, files);
    own(p, arena);
    for (size_t i = 0; i < n; ++i) {
        char *rel = dup_n(p, files[i].name, strlen(files[i].name));
        if (rel == NULL) return RP_EXIT_IO;
        for (char *c = rel; *c; ++c) {
            if (*c == '\\') *c = '/';       // CAB folders are written with '\'
        }
        if (!path_ok(p, rel, "file") || !add_file(p, rel, files[i].data, files[i].size)) return RP_EXIT_IO;
    }
    return p->nomem ? RP_EXIT_IO : RP_EXIT_OK;
}

// ---- a chain's setup program: its packages as <ID>.msi (RFC-0016 3) ---------------------------

static int plan_chain(plan_t *p, const uint8_t *data, size_t len) {
    rp_chain_entry_t *e = NULL;
    size_t n = 0;
    proven_err_t err = rp_chain_read(p->alloc, data, len, NULL, 0, &e, &n);
    if (err != PROVEN_OK) {
        rp_diag_error(RP_DIAG_BAD_PACKAGE, "'%s' is not a rubrapack setup program%s", p->path, err == PROVEN_ERR_NOT_FOUND ? "" : " (its payload is damaged)");
        return RP_EXIT_IO;
    }
    own(p, e);
    for (size_t i = 0; i < n; ++i) {
        if (!e[i].hash_ok) {
            rp_diag_error(RP_DIAG_BAD_PACKAGE, "'%s': package '%s' does not match its SHA-256", p->path, e[i].id);
            return RP_EXIT_IO;
        }
        size_t il = strlen(e[i].id);
        char *rel = dup_n(p, e[i].id, il + 4);
        if (rel == NULL) return RP_EXIT_IO;
        memcpy(rel + il, ".msi", 5);
        if (!path_ok(p, rel, "file") || !add_file(p, rel, e[i].data, (size_t)e[i].size)) return RP_EXIT_IO;
    }
    return p->nomem ? RP_EXIT_IO : RP_EXIT_OK;
}

// ---- MSIX: the payload at its paths (block hashes checked while reading) ----------------------

static int plan_msix(plan_t *p, const uint8_t *data, size_t len) {
    rp_msix_file_t *files = NULL;
    size_t n = 0;
    uint8_t *manifest = NULL;
    size_t ml = 0;
    const char *why = NULL;
    if (rp_msix_open(p->alloc, data, len, &p->read, &files, &n, &manifest, &ml, &why) != PROVEN_OK) {
        rp_diag_error(RP_DIAG_BAD_PACKAGE, "'%s' is not a valid MSIX package: %s", p->path, why ? why : "unreadable");
        return RP_EXIT_IO;
    }
    rp_mem_free(p->alloc, manifest);
    int rc = RP_EXIT_OK;
    for (size_t i = 0; i < n && rc == RP_EXIT_OK; ++i) {
        own(p, files[i].data);              // kept until the plan is written
        const uint8_t *fdata = files[i].data;
        files[i].data = NULL;
        char *rel = dup_n(p, files[i].name, strlen(files[i].name));
        if (rel == NULL) {
            rc = RP_EXIT_IO;
            break;
        }
        for (char *c = rel; *c; ++c) {
            if (*c == '\\') *c = '/';
        }
        if (!path_ok(p, rel, "file") || !add_file(p, rel, fdata, (size_t)files[i].size)) rc = RP_EXIT_IO;
    }
    for (size_t i = 0; i < n; ++i) {
        rp_mem_free(p->alloc, files[i].name);
        rp_mem_free(p->alloc, files[i].data);
    }
    rp_mem_free(p->alloc, files);
    return rc != RP_EXIT_OK || p->nomem ? RP_EXIT_IO : RP_EXIT_OK;
}

// ---- MSI -------------------------------------------------------------------------------------

typedef struct {
    const rp_msi_wtable_t *t;
    size_t                 cols[8];
} tab_t;

static bool tab(const rp_msi_wdb_t *db, const char *name, const char *const *cols, size_t n, tab_t *out) {
    memset(out, 0, sizeof *out);
    for (size_t i = 0; i < db->table_count; ++i) {
        if (strcmp(db->tables[i].name, name) == 0) out->t = &db->tables[i];
    }
    if (out->t == NULL) return false;
    for (size_t c = 0; c < n; ++c) {
        out->cols[c] = SIZE_MAX;
        for (size_t k = 0; k < out->t->column_count; ++k) {
            if (strcmp(out->t->columns[k].name, cols[c]) == 0) out->cols[c] = k;
        }
        if (out->cols[c] == SIZE_MAX) return false;
    }
    return true;
}

static const rp_msi_cell_t *at(const tab_t *t, size_t row, size_t c) {
    return &t->t->cells[row * t->t->column_count + t->cols[c]];
}

static char *cell_str(plan_t *p, const rp_msi_cell_t *c) {
    if (c->kind != RP_MSI_STR) return dup_n(p, "", 0);
    return dup_n(p, (const char *)c->bytes, c->len);
}

// "short|long" -> the long name (or the short one); a DefaultDir's "target:source" -> one side
// (`split`; a FileName has no such part, so a ':' there stays and is refused as a name).
static char *pick_name(plan_t *p, const char *dd, bool split, bool source, bool short_names) {
    const char *colon = split ? strchr(dd, ':') : NULL;
    const char *s = colon && source ? colon + 1 : dd;
    size_t n = colon && !source ? (size_t)(colon - dd) : strlen(s);
    const char *bar = memchr(s, '|', n);
    if (bar && !short_names) return dup_n(p, bar + 1, n - (size_t)(bar + 1 - s));
    if (bar) return dup_n(p, s, (size_t)(bar - s));
    return dup_n(p, s, n);
}

typedef struct {
    char *key, *parent, *dd;
    char *target, *source;      // resolved relative paths
    int   state;                // 0 new, 1 resolving, 2 done
} dir_t;

static dir_t *find_dir(dir_t *d, size_t n, const char *key) {
    for (size_t i = 0; i < n; ++i) {
        if (strcmp(d[i].key, key) == 0) return &d[i];
    }
    return NULL;
}

static bool resolve(plan_t *p, dir_t *d, size_t n, dir_t *x, unsigned depth, bool short_src) {
    if (x->state == 2) return true;
    if (x->state == 1 || depth > p->limits.max_depth) {
        rp_diag_error(RP_DIAG_BAD_PACKAGE, "'%s': directory '%s' is its own ancestor or nested too deep", p->path, x->key);
        return false;
    }
    x->state = 1;
    bool root = x->parent[0] == '\0' || strcmp(x->parent, x->key) == 0;
    if (root) {
        x->target = dup_n(p, "", 0);
        x->source = dup_n(p, "", 0);
    } else {
        dir_t *parent = find_dir(d, n, x->parent);
        if (parent == NULL) {
            rp_diag_error(RP_DIAG_BAD_PACKAGE, "'%s': directory '%s' has a parent '%s' that is not there", p->path, x->key, x->parent);
            return false;
        }
        if (!resolve(p, d, n, parent, depth + 1, short_src)) return false;
        bool parent_root = parent->parent[0] == '\0' || strcmp(parent->parent, parent->key) == 0;
        char *t = pick_name(p, x->dd, true, false, false), *s = pick_name(p, x->dd, true, true, short_src);
        if (t == NULL || s == NULL) return false;
        // "." is the parent itself; a standard folder right under the root keeps its key.
        x->target = strcmp(t, ".") == 0 ? (parent_root ? join(p, parent->target, x->key) : parent->target) : join(p, parent->target, t);
        x->source = strcmp(s, ".") == 0 ? parent->source : join(p, parent->source, s);
        if (x->target == NULL || x->source == NULL) return false;
        if (x->target[0] && !path_ok(p, x->target, "folder")) return false;
    }
    x->state = 2;
    return true;
}

static const uint16_t summary_name[] = { 5, 'S', 'u', 'm', 'm', 'a', 'r', 'y', 'I', 'n', 'f', 'o', 'r', 'm', 'a', 't', 'i', 'o', 'n' };

static int plan_msi(plan_t *p, const rp_cfb_t *cfb, const char *msi_dir) {
    rp_msi_t msi;
    const char *why = NULL;
    if (rp_msi_open(&msi, p->alloc, cfb, &p->read, &why) != PROVEN_OK) {
        rp_diag_error(RP_DIAG_BAD_PACKAGE, "'%s' is not a valid MSI database: %s", p->path, why ? why : "unreadable");
        return RP_EXIT_IO;
    }
    // Word Count (PID 15): bit 0 short source names, bit 1 compressed by default.
    int32_t word_count = 0;
    uint32_t sid;
    if (rp_cfb_find(cfb, 0, summary_name, sizeof summary_name / sizeof summary_name[0], &sid) == PROVEN_OK &&
        cfb->entries[sid].size <= p->limits.max_metadata) {
        size_t n = (size_t)cfb->entries[sid].size;
        uint8_t *buf = rp_mem_alloc(p->alloc, n + 1, 1);
        rp_suminfo_t si;
        if (buf && rp_cfb_read(cfb, sid, buf, n) == PROVEN_OK && rp_suminfo_parse(buf, n, &si) == PROVEN_OK) {
            for (size_t i = 0; i < si.count; ++i) {
                if (si.props[i].pid == 15 && si.props[i].type == RP_VT_I4) word_count = si.props[i].i;
            }
        }
        rp_mem_free(p->alloc, buf);
    }
    rp_msi_view_t view;
    int rc = RP_EXIT_IO;
    if (rp_msi_view(&msi, NULL, 0, &view) != PROVEN_OK) {
        rp_diag_error(RP_DIAG_BAD_PACKAGE, "'%s': a table cannot be read", p->path);
        rp_msi_close(&msi);
        return RP_EXIT_IO;
    }
    const rp_msi_wdb_t *db = &view.db;
    static const char *const dcols[] = { "Directory", "Directory_Parent", "DefaultDir" };
    static const char *const ccols[] = { "Component", "Directory_" };
    static const char *const fcols[] = { "File", "Component_", "FileName", "FileSize", "Attributes", "Sequence" };
    static const char *const mcols[] = { "DiskId", "LastSequence", "Cabinet" };
    static const char *const hcols[] = { "File_", "HashPart1", "HashPart2", "HashPart3", "HashPart4" };
    tab_t dt, ct, ft, mt, ht;
    bool has_hash = tab(db, "MsiFileHash", hcols, 5, &ht);
    if (!tab(db, "Directory", dcols, 3, &dt) || !tab(db, "Component", ccols, 2, &ct) || !tab(db, "File", fcols, 6, &ft) ||
        !tab(db, "Media", mcols, 3, &mt)) {
        rp_diag_error(RP_DIAG_BAD_PACKAGE, "'%s' has no Directory, Component, File and Media tables to extract from", p->path);
        goto out;
    }
    size_t nd = dt.t->row_count;
    dir_t *dirs = rp_mem_alloc(p->alloc, nd + 1, sizeof *dirs);
    if (dirs == NULL) goto out;
    own(p, dirs);
    for (size_t r = 0; r < nd; ++r) {
        dirs[r] = (dir_t){ cell_str(p, at(&dt, r, 0)), cell_str(p, at(&dt, r, 1)), cell_str(p, at(&dt, r, 2)), NULL, NULL, 0 };
        if (p->nomem) goto out;
    }
    for (size_t r = 0; r < nd; ++r) {
        if (!resolve(p, dirs, nd, &dirs[r], 0, word_count & 1)) goto out;
    }
    // Media in LastSequence order: a file belongs to the first media whose LastSequence >= it.
    size_t nm = mt.t->row_count;
    typedef struct { int32_t last; char *cab; rp_cab_file_t *files; size_t count; bool loaded; } media_t;
    media_t *media = rp_mem_alloc(p->alloc, nm + 1, sizeof *media);
    if (media == NULL) goto out;
    own(p, media);
    for (size_t r = 0; r < nm; ++r) {
        const rp_msi_cell_t *last = at(&mt, r, 1);
        media[r] = (media_t){ last->kind == RP_MSI_INT ? last->i : 0, at(&mt, r, 2)->kind == RP_MSI_STR ? cell_str(p, at(&mt, r, 2)) : NULL, NULL, 0, false };
    }
    for (size_t i = 1; i < nm; ++i) {
        for (size_t j = i; j > 0 && media[j - 1].last > media[j].last; --j) {
            media_t t = media[j];
            media[j] = media[j - 1];
            media[j - 1] = t;
        }
    }
    for (size_t r = 0; r < ft.t->row_count; ++r) {
        char *key = cell_str(p, at(&ft, r, 0)), *comp = cell_str(p, at(&ft, r, 1)), *fname = cell_str(p, at(&ft, r, 2));
        const rp_msi_cell_t *fsize = at(&ft, r, 3), *fattr = at(&ft, r, 4), *fseq = at(&ft, r, 5);
        if (p->nomem) goto out;
        const char *dkey = NULL;
        for (size_t c = 0; c < ct.t->row_count && dkey == NULL; ++c) {
            const rp_msi_cell_t *k = at(&ct, c, 0);
            if (k->kind == RP_MSI_STR && k->len == strlen(comp) && memcmp(k->bytes, comp, k->len) == 0) dkey = cell_str(p, at(&ct, c, 1));
        }
        dir_t *dir = dkey ? find_dir(dirs, nd, dkey) : NULL;
        if (dir == NULL) {
            rp_diag_error(RP_DIAG_BAD_PACKAGE, "'%s': file '%s' has no component or directory", p->path, key);
            goto out;
        }
        char *tname = pick_name(p, fname, false, false, false), *sname = pick_name(p, fname, false, true, word_count & 1);
        char *rel = join(p, dir->target, tname);
        if (rel == NULL || !path_ok(p, rel, "file")) goto out;
        int32_t seq = fseq->kind == RP_MSI_INT ? fseq->i : 0, attr = fattr->kind == RP_MSI_INT ? fattr->i : 0;
        size_t mi = 0;
        while (mi < nm && media[mi].last < seq) ++mi;
        bool compressed = (attr & 0x4000) || (!(attr & 0x2000) && (word_count & 2));
        const uint8_t *data = NULL;
        size_t size = 0;
        if (compressed) {
            if (mi == nm || media[mi].cab == NULL) {
                rp_diag_error(RP_DIAG_BAD_PACKAGE, "'%s': compressed file '%s' (sequence %ld) has no cabinet", p->path, key, (long)seq);
                goto out;
            }
            media_t *m = &media[mi];
            if (!m->loaded) {
                uint8_t *cab = NULL;
                size_t cab_len = 0;
                if (m->cab[0] == '#') {         // an embedded stream
                    uint16_t packed[32];
                    size_t plen;
                    uint32_t id;
                    if (rp_msi_stream_name(m->cab + 1, false, packed, &plen) != PROVEN_OK ||
                        rp_cfb_find(cfb, 0, packed, plen, &id) != PROVEN_OK || cfb->entries[id].size > MAX_INPUT) {
                        rp_diag_error(RP_DIAG_BAD_PACKAGE, "'%s': the cabinet stream '%s' is not there", p->path, m->cab + 1);
                        goto out;
                    }
                    cab_len = (size_t)cfb->entries[id].size;
                    cab = rp_mem_alloc(p->alloc, cab_len + 1, 1);
                    if (cab == NULL || rp_cfb_read(cfb, id, cab, cab_len) != PROVEN_OK) {
                        rp_mem_free(p->alloc, cab);
                        goto out;
                    }
                } else {                        // a file next to the package
                    if (name_problem(m->cab, strlen(m->cab))) {
                        rp_diag_error(RP_DIAG_BAD_PACKAGE, "'%s': cabinet name '%s' is not a plain file name", p->path, m->cab);
                        goto out;
                    }
                    char *cp = join(p, msi_dir, m->cab);
                    if (cp == NULL || rp_pal_read_file(p->alloc, cp, MAX_INPUT, &cab, &cab_len) != PROVEN_OK) {
                        rp_diag_error(RP_DIAG_INPUT, "cannot read the cabinet '%s' next to '%s'", m->cab, p->path);
                        goto out;
                    }
                }
                own(p, cab);
                uint8_t *arena = NULL;
                if (rp_cab_read(p->alloc, cab, cab_len, &p->read, &m->files, &m->count, &arena) != PROVEN_OK) {
                    rp_diag_error(RP_DIAG_BAD_PACKAGE, "'%s': cabinet '%s' is damaged or not stored/MSZIP", p->path, m->cab);
                    goto out;
                }
                own(p, m->files);
                own(p, arena);
                m->loaded = true;
            }
            size_t k = 0;
            while (k < m->count && strcmp(m->files[k].name, key) != 0) ++k;
            if (k == m->count) {
                rp_diag_error(RP_DIAG_BAD_PACKAGE, "'%s': file '%s' is not in cabinet '%s'", p->path, key, m->cab);
                goto out;
            }
            data = m->files[k].data;
            size = m->files[k].size;
        } else {                                // uncompressed: the source tree next to the package
            char *srel = join(p, dir->source, sname);
            if (srel == NULL || !path_ok(p, srel, "source file")) goto out;
            char *sp = join(p, msi_dir, srel);
            uint8_t *buf = NULL;
            if (sp == NULL || rp_pal_read_file(p->alloc, sp, MAX_INPUT, &buf, &size) != PROVEN_OK) {
                rp_diag_error(RP_DIAG_INPUT, "cannot read the uncompressed file '%s' next to '%s'", srel, p->path);
                goto out;
            }
            own(p, buf);
            data = buf;
        }
        if (fsize->kind == RP_MSI_INT && (uint64_t)(uint32_t)fsize->i != size) {
            rp_diag_error(RP_DIAG_BAD_PACKAGE, "'%s': file '%s' is %zu bytes, the File table says %lu", p->path, key, size,
                          (unsigned long)(uint32_t)fsize->i);
            goto out;
        }
        for (size_t h = 0; has_hash && h < ht.t->row_count; ++h) {
            const rp_msi_cell_t *hk = at(&ht, h, 0);
            if (hk->kind != RP_MSI_STR || hk->len != strlen(key) || memcmp(hk->bytes, key, hk->len) != 0) continue;
            uint8_t md5[16];
            rp_md5(data, size, md5);
            bool same = true;
            for (int part = 0; part < 4; ++part) {
                const rp_msi_cell_t *v = at(&ht, h, (size_t)part + 1);
                uint32_t want = v->kind == RP_MSI_INT ? (uint32_t)v->i : 0, got = (uint32_t)md5[part * 4] | (uint32_t)md5[part * 4 + 1] << 8 |
                                                                                 (uint32_t)md5[part * 4 + 2] << 16 | (uint32_t)md5[part * 4 + 3] << 24;
                same &= want == got;
            }
            if (!same) {
                rp_diag_error(RP_DIAG_BAD_PACKAGE, "'%s': file '%s' does not match its MsiFileHash", p->path, key);
                goto out;
            }
        }
        if (!add_file(p, rel, data, size)) goto out;
    }
    rc = p->nomem ? RP_EXIT_IO : RP_EXIT_OK;
out:
    rp_msi_view_free(&msi, &view);
    rp_msi_close(&msi);
    return rc;
}

static bool ends_with_ci(const char *s, const char *suffix) {
    size_t n = strlen(s), m = strlen(suffix);
    if (n < m) return false;
    for (size_t k = 0; k < m; ++k) {
        char a = s[n - m + k];
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (a != suffix[k]) return false;
    }
    return true;
}

static bool parse_u64(const char *s, uint64_t *out) {
    char *end = NULL;
    if (s == NULL || *s < '0' || *s > '9') return false;
    unsigned long long v = strtoull(s, &end, 10);
    if (end == NULL || *end != '\0' || v == 0) return false;
    *out = v;
    return true;
}

int rp_cmd_extract(int argc, char **argv) {
    const char *input = NULL, *dest = NULL;
    rp_limits_t limits = rp_limits_default();
    for (int i = 2; i < argc; ++i) {
        const char *a = argv[i], *next = i + 1 < argc ? argv[i + 1] : NULL;
        if (strcmp(a, "-d") == 0 && next) {
            dest = next;
            ++i;
        } else if (strcmp(a, "--limit-entries") == 0 && parse_u64(next, &limits.max_entries)) {
            ++i;
        } else if (strcmp(a, "--limit-bytes") == 0 && parse_u64(next, &limits.max_output)) {
            ++i;
        } else if (a[0] == '-' || input) {
            input = NULL;
            dest = NULL;
            break;
        } else {
            input = a;
        }
    }
    if (input == NULL || dest == NULL || !(ends_with_ci(input, ".msi") || ends_with_ci(input, ".cab") || ends_with_ci(input, ".msix") ||
                                             ends_with_ci(input, ".msixbundle") || ends_with_ci(input, ".exe"))) {
        rp_diag_error(RP_DIAG_EXTRA_ARGUMENT, "usage: rubrapack extract <file.msi|file.msix|file.msixbundle|file.cab|setup.exe> -d <new dir> [--limit-entries N] [--limit-bytes N]");
        return RP_EXIT_USAGE;
    }
    proven_allocator_t heap = proven_heap_allocator();
    rp_limits_t read = rp_limits_default();
    if (limits.max_entries > read.max_entries) read.max_entries = limits.max_entries;
    if (limits.max_output > read.max_output) read.max_output = limits.max_output;
    plan_t p = { .alloc = heap, .limits = limits, .read = read, .path = input };
    uint8_t *data = NULL;
    size_t len = 0;
    if (rp_pal_read_file(heap, input, MAX_INPUT, &data, &len) != PROVEN_OK) {
        rp_diag_error(RP_DIAG_INPUT, "cannot read '%s'", input);
        return RP_EXIT_IO;
    }
    int rc;
    if (ends_with_ci(input, ".cab")) {
        rc = plan_cab(&p, data, len);
    } else if (ends_with_ci(input, ".exe")) {
        rc = plan_chain(&p, data, len);
    } else if (ends_with_ci(input, ".msix") || ends_with_ci(input, ".msixbundle")) {
        rc = plan_msix(&p, data, len);
    } else {
        rp_cfb_t cfb;
        const char *why = NULL;
        if (rp_cfb_open(&cfb, heap, data, len, &read, &why) != PROVEN_OK) {
            rp_diag_error(RP_DIAG_BAD_PACKAGE, "'%s' is not a valid package: %s", input, why ? why : "unreadable");
            rc = RP_EXIT_IO;
        } else {
            // The package's folder, for external cabinets and uncompressed files.
            const char *slash = strrchr(input, '/');
#if defined(_WIN32)
            const char *bs = strrchr(input, '\\');
            if (bs && (!slash || bs > slash)) slash = bs;
#endif
            char *dir = slash ? dup_n(&p, input, (size_t)(slash - input)) : dup_n(&p, ".", 1);
            rc = dir ? plan_msi(&p, &cfb, dir) : RP_EXIT_IO;
            if (rc == RP_EXIT_OK) rc = write_plan(&p, dest);
            rp_cfb_close(&cfb);
            goto done;
        }
    }
    if (rc == RP_EXIT_OK) rc = write_plan(&p, dest);
done:
    if (p.nomem && rc == RP_EXIT_OK) rc = RP_EXIT_IO;
    for (size_t i = 0; i < p.nowned; ++i) rp_mem_free(heap, p.owned[i]);
    rp_mem_free(heap, p.owned);
    rp_mem_free(heap, p.items);
    rp_mem_free(heap, data);
    return rc;
}
