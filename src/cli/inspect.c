// src/cli/inspect.c - `rubrapack inspect <file.msi> [table|--summary|--files|--streams]` (RFC-0001 7).
// Results go to stdout as UTF-8 (IDT for tables, as MsiDatabaseExport writes it); diagnostics
// to stderr.

#include "rubrapack/buf.h"
#include "rubrapack/cfb.h"
#include "rubrapack/diag.h"
#include "rubrapack/inspect.h"
#include "rubrapack/mem.h"
#include "rubrapack/msi.h"
#include "rubrapack/pal.h"
#include "rubrapack/suminfo.h"

#include <string.h>

#include "proven/heap.h"

enum { MAX_INPUT = 1u << 30 };

static const uint16_t summary_name[] = { 5, 'S', 'u', 'm', 'm', 'a', 'r', 'y', 'I', 'n', 'f', 'o', 'r', 'm', 'a', 't', 'i', 'o', 'n' };

static int emit(const uint8_t *p, size_t n) {
    return rp_pal_write(RP_OUT_STDOUT, p, n) == PROVEN_OK ? RP_EXIT_OK : RP_EXIT_IO;
}

static int emit_buf(rp_buf_t *b) {
    uint8_t *p;
    size_t n;
    if (rp_buf_take(b, &p, &n) != PROVEN_OK) return RP_EXIT_IO;
    int rc = emit(p, n);
    rp_mem_free(b->alloc, p);
    return rc;
}

// Reads the summary stream; *si is empty when the package has none.
static proven_err_t load_summary(const rp_cfb_t *cfb, proven_allocator_t alloc, uint8_t **buf, rp_suminfo_t *si) {
    uint32_t id;
    *buf = NULL;
    memset(si, 0, sizeof *si);
    if (rp_cfb_find(cfb, 0, summary_name, sizeof summary_name / sizeof summary_name[0], &id) != PROVEN_OK) {
        return PROVEN_OK;
    }
    size_t n = (size_t)cfb->entries[id].size;
    *buf = rp_mem_alloc(alloc, n, 1);
    if (*buf == NULL) return PROVEN_ERR_NOMEM;
    proven_err_t err = rp_cfb_read(cfb, id, *buf, n);
    if (err == PROVEN_OK) err = rp_suminfo_parse(*buf, n, si);
    return err;
}

static int overview(const rp_msi_t *msi, const rp_suminfo_t *si, proven_allocator_t alloc) {
    rp_buf_t b = rp_buf_new(alloc, (size_t)1 << 26);
    rp_buf_puts(&b, "code page: ");
    rp_buf_long(&b, msi->codepage);
    rp_buf_puts(&b, "\nstrings: ");
    rp_buf_long(&b, (long long)msi->string_count - 1);
    rp_buf_puts(&b, msi->long_refs ? " (3-byte references)\ntables: " : "\ntables: ");
    rp_buf_long(&b, (long long)msi->table_count);
    rp_buf_puts(&b, "\n");
    for (size_t t = 0; t < msi->table_count; ++t) {
        const uint8_t *p;
        size_t n;
        rp_msi_rows_t rows;
        (void)rp_msi_string(msi, msi->tables[t].name, &p, &n);
        rp_buf_puts(&b, "  ");
        rp_buf_put(&b, p, n);
        if (rp_msi_read_rows(msi, t, &rows) == PROVEN_OK) {
            rp_buf_puts(&b, "  rows=");
            rp_buf_long(&b, (long long)rows.row_count);
            rp_msi_rows_free(msi, &rows);
        } else {
            rp_buf_puts(&b, "  rows=unreadable");
        }
        rp_buf_puts(&b, " columns=");
        rp_buf_long(&b, (long long)msi->tables[t].column_count);
        rp_buf_puts(&b, "\n");
    }
    rp_buf_puts(&b, "summary:\n");
    for (size_t k = 0; k < si->count; ++k) {
        const rp_suminfo_prop_t *p = &si->props[k];
        rp_buf_puts(&b, "  ");
        rp_buf_long(&b, p->pid);
        rp_buf_puts(&b, " = ");
        if (p->type == RP_VT_LPSTR) rp_buf_put(&b, p->str, p->str_len);
        else if (p->type == RP_VT_FILETIME) {
            char t[20];
            rp_filetime_text(p->filetime, t);
            rp_buf_puts(&b, t);
            rp_buf_puts(&b, " UTC");
        }
        else rp_buf_long(&b, p->i);
        rp_buf_puts(&b, "\n");
    }
    return emit_buf(&b);
}

static int streams(const rp_cfb_t *cfb, proven_allocator_t alloc) {
    size_t n = 0;
    if (rp_cfb_children(cfb, 0, NULL, 0, &n) != PROVEN_OK) return RP_EXIT_IO;
    uint32_t *ids = rp_mem_alloc(alloc, n, sizeof *ids);
    if (ids == NULL || rp_cfb_children(cfb, 0, ids, n, &n) != PROVEN_OK) {
        rp_mem_free(alloc, ids);
        return RP_EXIT_IO;
    }
    rp_buf_t b = rp_buf_new(alloc, (size_t)1 << 26);
    for (size_t i = 0; i < n; ++i) {
        const rp_cfb_entry_t *e = &cfb->entries[ids[i]];
        char name[160];
        bool table = false;
        if (rp_msi_unpack_name(e->name, e->name_len, name, sizeof name, &table) != PROVEN_OK) strcpy(name, "?");
        if (name[0] == 5) name[0] = '!';    // \005SummaryInformation, printable
        rp_buf_puts(&b, table ? "table  " : "stream ");
        rp_buf_long(&b, (long long)e->size);
        rp_buf_byte(&b, '\t');
        rp_buf_puts(&b, name);
        rp_buf_byte(&b, '\n');
    }
    rp_mem_free(alloc, ids);
    return emit_buf(&b);
}

// ---- --files -------------------------------------------------------------------------------

typedef struct {
    rp_msi_rows_t rows;
    size_t        table;
    bool          present;
} tab_t;

static bool load_table(const rp_msi_t *msi, const char *name, tab_t *t) {
    memset(t, 0, sizeof *t);
    if (rp_msi_find_table(msi, name, &t->table) != PROVEN_OK) return true;     // absent is fine
    t->present = true;
    return rp_msi_read_rows(msi, t->table, &t->rows) == PROVEN_OK;
}

static int column_of(const rp_msi_t *msi, const tab_t *t, const char *name) {
    if (!t->present) return -1;
    const rp_msi_table_t *tb = &msi->tables[t->table];
    size_t n = strlen(name);
    for (size_t c = 0; c < tb->column_count; ++c) {
        const uint8_t *p;
        size_t len;
        if (rp_msi_string(msi, tb->columns[c].name, &p, &len) == PROVEN_OK && len == n && memcmp(p, name, n) == 0) {
            return (int)c;
        }
    }
    return -1;
}

static rp_msi_value_t cell(const tab_t *t, size_t row, int col) {
    return t->rows.cells[row * t->rows.column_count + (size_t)col];
}

// The long name of a "short|long" or "target:source" name field.
static void long_name(const rp_msi_t *msi, rp_msi_value_t v, const uint8_t **p, size_t *n) {
    *p = (const uint8_t *)"";
    *n = 0;
    if (v.kind != RP_MSI_STR || rp_msi_string(msi, v.s, p, n) != PROVEN_OK) return;
    const uint8_t *colon = memchr(*p, ':', *n);
    if (colon) *n = (size_t)(colon - *p);
    const uint8_t *bar = memchr(*p, '|', *n);
    if (bar) {
        *n -= (size_t)(bar + 1 - *p);
        *p = bar + 1;
    }
}

// Directory keys that Windows Installer resolves itself (system folder properties).
static bool standard_folder(const uint8_t *p, size_t n) {
    static const char *const names[] = {
        "AdminToolsFolder", "AppDataFolder", "CommonAppDataFolder", "CommonFiles64Folder", "CommonFilesFolder",
        "DesktopFolder", "FavoritesFolder", "FontsFolder", "LocalAppDataFolder", "MyPicturesFolder",
        "NetHoodFolder", "PersonalFolder", "PrintHoodFolder", "ProgramFiles64Folder", "ProgramFilesFolder",
        "ProgramMenuFolder", "RecentFolder", "SendToFolder", "StartMenuFolder", "StartupFolder",
        "System16Folder", "System64Folder", "SystemFolder", "TempFolder", "TemplateFolder",
        "WindowsFolder", "WindowsVolume",
    };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; ++i) {
        if (strlen(names[i]) == n && memcmp(names[i], p, n) == 0) return true;
    }
    return false;
}

// Appends the target path of directory row `row` (parents first). Roots - TARGETDIR and the
// system folder properties - print as [Name].
static bool dir_path(const rp_msi_t *msi, const tab_t *dir, int c_key, int c_parent, int c_default, size_t row,
                     rp_buf_t *b, size_t depth) {
    if (depth > dir->rows.row_count) return false;      // the parent chain loops
    rp_msi_value_t parent = cell(dir, row, c_parent), key = cell(dir, row, c_key);
    const uint8_t *p;
    size_t n;
    (void)rp_msi_string(msi, key.s, &p, &n);
    bool root = parent.kind != RP_MSI_STR || parent.s == key.s || standard_folder(p, n);
    if (root) {
        rp_buf_byte(b, '[');
        rp_buf_put(b, p, n);
        rp_buf_byte(b, ']');
        return true;
    }
    size_t prow = SIZE_MAX;
    for (size_t r = 0; r < dir->rows.row_count; ++r) {
        rp_msi_value_t k = cell(dir, r, c_key);
        if (k.kind == RP_MSI_STR && k.s == parent.s) prow = r;
    }
    if (prow == SIZE_MAX) return false;
    if (!dir_path(msi, dir, c_key, c_parent, c_default, prow, b, depth + 1)) return false;
    long_name(msi, cell(dir, row, c_default), &p, &n);
    if (!(n == 1 && p[0] == '.')) {
        rp_buf_byte(b, '\\');
        rp_buf_put(b, p, n);
    }
    return true;
}

static int files(const rp_msi_t *msi, proven_allocator_t alloc) {
    tab_t file, comp, dir;
    int rc = RP_EXIT_OK;
    if (!load_table(msi, "File", &file) || !load_table(msi, "Component", &comp) || !load_table(msi, "Directory", &dir)) {
        rc = RP_EXIT_IO;
    }
    int f_key = column_of(msi, &file, "File"), f_comp = column_of(msi, &file, "Component_"),
        f_name = column_of(msi, &file, "FileName"), f_size = column_of(msi, &file, "FileSize");
    int c_key = column_of(msi, &comp, "Component"), c_dir = column_of(msi, &comp, "Directory_");
    int d_key = column_of(msi, &dir, "Directory"), d_parent = column_of(msi, &dir, "Directory_Parent"),
        d_default = column_of(msi, &dir, "DefaultDir");
    if (rc == RP_EXIT_OK && file.present &&
        (f_key < 0 || f_comp < 0 || f_name < 0 || f_size < 0 || c_key < 0 || c_dir < 0 || d_key < 0 || d_parent < 0 ||
         d_default < 0)) {
        rp_diag_error(RP_DIAG_BAD_PACKAGE, "File, Component or Directory table lacks a standard column");
        rc = RP_EXIT_IO;
    }
    // One line per file: path, size, File key, Component; sorted by bytes.
    size_t n = file.present ? file.rows.row_count : 0;
    rp_buf_t *lines = rp_mem_alloc(alloc, n, sizeof *lines);
    for (size_t r = 0; rc == RP_EXIT_OK && r < n; ++r) {
        lines[r] = rp_buf_new(alloc, 1u << 16);
        rp_msi_value_t compref = cell(&file, r, f_comp);
        size_t crow = SIZE_MAX;
        for (size_t k = 0; k < comp.rows.row_count; ++k) {
            rp_msi_value_t v = cell(&comp, k, c_key);
            if (v.kind == RP_MSI_STR && compref.kind == RP_MSI_STR && v.s == compref.s) crow = k;
        }
        size_t drow = SIZE_MAX;
        if (crow != SIZE_MAX) {
            rp_msi_value_t dref = cell(&comp, crow, c_dir);
            for (size_t k = 0; k < dir.rows.row_count; ++k) {
                rp_msi_value_t v = cell(&dir, k, d_key);
                if (v.kind == RP_MSI_STR && dref.kind == RP_MSI_STR && v.s == dref.s) drow = k;
            }
        }
        if (drow == SIZE_MAX || !dir_path(msi, &dir, d_key, d_parent, d_default, drow, &lines[r], 0)) {
            rp_diag_error(RP_DIAG_BAD_PACKAGE, "a file's component or directory chain is broken");
            rc = RP_EXIT_IO;
            break;
        }
        const uint8_t *p;
        size_t len;
        long_name(msi, cell(&file, r, f_name), &p, &len);
        rp_buf_byte(&lines[r], '\\');
        rp_buf_put(&lines[r], p, len);
        rp_buf_byte(&lines[r], '\t');
        rp_msi_value_t size = cell(&file, r, f_size);
        if (size.kind == RP_MSI_INT) rp_buf_long(&lines[r], size.i);
        rp_buf_byte(&lines[r], '\t');
        (void)rp_msi_string(msi, cell(&file, r, f_key).s, &p, &len);
        rp_buf_put(&lines[r], p, len);
        rp_buf_byte(&lines[r], '\t');
        (void)rp_msi_string(msi, compref.s, &p, &len);
        rp_buf_put(&lines[r], p, len);
        rp_buf_byte(&lines[r], '\n');
    }
    if (rc == RP_EXIT_OK) {
        // insertion sort by bytes (file counts are modest)
        for (size_t i = 1; i < n; ++i) {
            for (size_t j = i; j > 0; --j) {
                rp_buf_t *a = &lines[j - 1], *b = &lines[j];
                size_t m = a->len < b->len ? a->len : b->len;
                int c = memcmp(a->data, b->data, m);
                if (c < 0 || (c == 0 && a->len <= b->len)) break;
                rp_buf_t t = *a;
                *a = *b;
                *b = t;
            }
        }
        rp_buf_t out = rp_buf_new(alloc, (size_t)1 << 26);
        for (size_t i = 0; i < n; ++i) rp_buf_put(&out, lines[i].data, lines[i].len);
        rc = emit_buf(&out);
    }
    for (size_t i = 0; lines && i < n; ++i) rp_buf_free(&lines[i]);
    rp_mem_free(alloc, lines);
    rp_msi_rows_free(msi, &file.rows);
    rp_msi_rows_free(msi, &comp.rows);
    rp_msi_rows_free(msi, &dir.rows);
    return rc;
}

int rp_cmd_inspect(int argc, char **argv) {
    if (argc < 3 || argc > 4) {
        rp_diag_error(RP_DIAG_EXTRA_ARGUMENT, "usage: rubrapack inspect <file.msi> [table|--summary|--files|--streams]");
        return RP_EXIT_USAGE;
    }
    const char *path = argv[2], *what = argc == 4 ? argv[3] : NULL;
    if (what && what[0] == '-' && strcmp(what, "--summary") != 0 && strcmp(what, "--streams") != 0 &&
        strcmp(what, "--files") != 0) {
        rp_diag_error(RP_DIAG_EXTRA_ARGUMENT, "unknown inspect option (use a table name, --summary, --files or --streams)");
        return RP_EXIT_USAGE;
    }

    proven_allocator_t heap = proven_heap_allocator();
    rp_limits_t limits = rp_limits_default();
    uint8_t *data = NULL, *sum = NULL;
    size_t len = 0;
    proven_err_t err = rp_pal_read_file(heap, path, MAX_INPUT, &data, &len);
    if (err != PROVEN_OK) {
        rp_diag_error(RP_DIAG_INPUT, "cannot read '%s' (%s)", path,
                      err == PROVEN_ERR_NOT_FOUND ? "not found" : err == PROVEN_ERR_OUT_OF_BOUNDS ? "too large" : "read error");
        return RP_EXIT_IO;
    }
    rp_cfb_t cfb;
    rp_msi_t msi;
    const char *why = NULL;
    int rc = RP_EXIT_OK;
    if (rp_cfb_open(&cfb, heap, data, len, &limits, &why) != PROVEN_OK) {
        rp_diag_error(RP_DIAG_BAD_PACKAGE, "'%s' is not a valid package: %s", path, why ? why : "unreadable");
        rp_mem_free(heap, data);
        return RP_EXIT_IO;
    }
    if (what && strcmp(what, "--streams") == 0) {
        rc = streams(&cfb, heap);
        rp_cfb_close(&cfb);
        rp_mem_free(heap, data);
        return rc;
    }
    if (rp_msi_open(&msi, heap, &cfb, &limits, &why) != PROVEN_OK) {
        rp_diag_error(RP_DIAG_BAD_PACKAGE, "'%s' is not a valid MSI database: %s", path, why ? why : "unreadable");
        rp_cfb_close(&cfb);
        rp_mem_free(heap, data);
        return RP_EXIT_IO;
    }
    rp_suminfo_t si;
    if (load_summary(&cfb, heap, &sum, &si) != PROVEN_OK) {
        rp_diag_error(RP_DIAG_BAD_PACKAGE, "'%s' has an unreadable summary information stream", path);
        rc = RP_EXIT_IO;
    } else if (what == NULL) {
        rc = overview(&msi, &si, heap);
    } else if (strcmp(what, "--files") == 0) {
        rc = files(&msi, heap);
    } else if (strcmp(what, "--summary") == 0) {
        uint8_t *out;
        size_t n;
        if (rp_suminfo_export_idt(&si, heap, &out, &n) != PROVEN_OK) {
            rp_diag_error(RP_DIAG_BAD_PACKAGE, "summary information cannot be shown as IDT yet");
            rc = RP_EXIT_IO;
        } else {
            rc = emit(out, n);
            rp_mem_free(heap, out);
        }
    } else {
        size_t t;
        uint8_t *out;
        size_t n;
        if (strcmp(what, "_ForceCodepage") == 0) {
            if (rp_msi_export_codepage(&msi, &out, &n) == PROVEN_OK) {
                rc = emit(out, n);
                rp_mem_free(heap, out);
            }
        } else if (rp_msi_find_table(&msi, what, &t) != PROVEN_OK) {
            rp_diag_error(RP_DIAG_NO_TABLE, "no table '%s' in '%s'", what, path);
            rc = RP_EXIT_USAGE;
        } else if (rp_msi_export_idt(&msi, t, &out, &n) != PROVEN_OK) {
            rp_diag_error(RP_DIAG_BAD_PACKAGE, "table '%s' is unreadable", what);
            rc = RP_EXIT_IO;
        } else {
            rc = emit(out, n);
            rp_mem_free(heap, out);
        }
    }
    rp_mem_free(heap, sum);
    rp_msi_close(&msi);
    rp_cfb_close(&cfb);
    rp_mem_free(heap, data);
    return rc;
}
