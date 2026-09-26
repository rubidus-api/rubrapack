// src/cli/inspect.c - `rubrapack inspect <file.msi> [table|--summary|--streams]` (RFC-0001 7).
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
        else if (p->type == RP_VT_FILETIME) rp_buf_puts(&b, "(date)");
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

int rp_cmd_inspect(int argc, char **argv) {
    if (argc < 3 || argc > 4) {
        rp_diag_error(RP_DIAG_EXTRA_ARGUMENT, "usage: rubrapack inspect <file.msi> [table|--summary|--streams]");
        return RP_EXIT_USAGE;
    }
    const char *path = argv[2], *what = argc == 4 ? argv[3] : NULL;
    if (what && strcmp(what, "--files") == 0) {
        rp_diag_error(RP_DIAG_NOT_IMPLEMENTED, "inspect --files is not implemented yet");
        return RP_EXIT_USAGE;
    }
    if (what && what[0] == '-' && strcmp(what, "--summary") != 0 && strcmp(what, "--streams") != 0) {
        rp_diag_error(RP_DIAG_EXTRA_ARGUMENT, "unknown inspect option (use a table name, --summary or --streams)");
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
