// src/cli/json.c - `inspect ... --json` (RFC-0025): an MSI's tables, or one table, or its summary
// information, as JSON on stdout. A table is {"name", "columns": [{"name", "type", "key",
// "nullable"}], "rows": [[...]]}; a cell is a string, an integer, null, or true for a stream
// (binary column) that is there - its bytes are not shown.

#include "rubrapack/diag.h"
#include "rubrapack/inspect.h"
#include "rubrapack/json.h"
#include "rubrapack/limits.h"
#include "rubrapack/mem.h"
#include "rubrapack/msi.h"
#include "rubrapack/pal.h"
#include "rubrapack/suminfo.h"

#include <stdio.h>
#include <string.h>

#include "proven/heap.h"

void rp_json_str(rp_buf_t *b, const char *s, size_t n) {
    rp_buf_byte(b, '"');
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') {
            rp_buf_byte(b, '\\');
            rp_buf_byte(b, c);
        } else if (c == '\n') {
            rp_buf_puts(b, "\\n");
        } else if (c == '\r') {
            rp_buf_puts(b, "\\r");
        } else if (c == '\t') {
            rp_buf_puts(b, "\\t");
        } else if (c < 0x20 || c == 0x7F) {
            char u[8];
            snprintf(u, sizeof u, "\\u%04x", c);
            rp_buf_puts(b, u);
        } else {
            rp_buf_byte(b, c);
        }
    }
    rp_buf_byte(b, '"');
}

static void table_json(rp_buf_t *b, const rp_msi_wtable_t *t) {
    rp_buf_puts(b, "{\"name\": ");
    rp_json_str(b, t->name, strlen(t->name));
    rp_buf_puts(b, ", \"columns\": [");
    for (size_t c = 0; c < t->column_count; ++c) {
        uint16_t ty = t->columns[c].type;
        const char *kind = !(ty & RP_MSI_COL_STRING) ? "integer" : (ty & RP_MSI_COL_NONBINARY) ? "string" : "binary";
        rp_buf_puts(b, c ? ", {\"name\": " : "{\"name\": ");
        rp_json_str(b, t->columns[c].name, strlen(t->columns[c].name));
        rp_buf_puts(b, ", \"type\": \"");
        rp_buf_puts(b, kind);
        rp_buf_puts(b, (ty & RP_MSI_COL_KEY) ? "\", \"key\": true" : "\", \"key\": false");
        rp_buf_puts(b, (ty & RP_MSI_COL_NULLABLE) ? ", \"nullable\": true}" : ", \"nullable\": false}");
    }
    rp_buf_puts(b, "], \"rows\": [");
    for (size_t r = 0; r < t->row_count; ++r) {
        rp_buf_puts(b, r ? ",\n  [" : "\n  [");
        for (size_t c = 0; c < t->column_count; ++c) {
            const rp_msi_cell_t *v = &t->cells[r * t->column_count + c];
            if (c) rp_buf_puts(b, ", ");
            if (v->kind == RP_MSI_STR) rp_json_str(b, (const char *)v->bytes, v->len);
            else if (v->kind == RP_MSI_INT) rp_buf_long(b, v->i);
            else if (v->kind == RP_MSI_BINARY) rp_buf_puts(b, "true");
            else rp_buf_puts(b, "null");
        }
        rp_buf_byte(b, ']');
    }
    rp_buf_puts(b, t->row_count ? "\n]}" : "]}");
}

static const char *const summary_names[] = { [1] = "Codepage", [2] = "Title", [3] = "Subject", [4] = "Author", [5] = "Keywords",
                                             [6] = "Comments", [7] = "Template", [8] = "LastSavedBy", [9] = "RevisionNumber",
                                             [11] = "LastPrinted", [12] = "CreateTime", [13] = "LastSaveTime", [14] = "PageCount",
                                             [15] = "WordCount", [16] = "CharacterCount", [18] = "AppName", [19] = "Security" };

int rp_inspect_json(const char *path, const char *what) {
    proven_allocator_t heap = proven_heap_allocator();
    rp_limits_t limits = rp_limits_default();
    rp_pkg_t p;
    int rc = rp_pkg_open(heap, path, &limits, &p);
    if (rc != RP_EXIT_OK) {
        rp_pkg_close(heap, &p);
        return rc;
    }
    rp_buf_t b = rp_buf_new(heap, (size_t)1 << 30);
    const rp_msi_wdb_t *db = &p.view.db;
    if (what && strcmp(what, "--summary") == 0) {
        rp_suminfo_t si;
        if (db->summary == NULL || rp_suminfo_parse(db->summary, db->summary_len, &si) != PROVEN_OK) {
            rp_diag_error(RP_DIAG_BAD_PACKAGE, "'%s' has no readable summary information", path);
            rc = RP_EXIT_IO;
        } else {
            rp_buf_puts(&b, "{\"summary\": [");
            for (size_t i = 0; i < si.count; ++i) {
                const rp_suminfo_prop_t *x = &si.props[i];
                rp_buf_puts(&b, i ? ",\n  {\"pid\": " : "\n  {\"pid\": ");
                rp_buf_long(&b, x->pid);
                const char *nm = x->pid < sizeof summary_names / sizeof summary_names[0] ? summary_names[x->pid] : NULL;
                if (nm) {
                    rp_buf_puts(&b, ", \"name\": \"");
                    rp_buf_puts(&b, nm);
                    rp_buf_byte(&b, '"');
                }
                rp_buf_puts(&b, ", \"value\": ");
                if (x->type == RP_VT_LPSTR) rp_json_str(&b, (const char *)x->str, x->str_len);
                else if (x->type == RP_VT_I4 || x->type == 2) rp_buf_long(&b, x->i);
                else rp_buf_long(&b, (long long)x->filetime);     // FILETIME: 100 ns since 1601
                rp_buf_byte(&b, '}');
            }
            rp_buf_puts(&b, si.count ? "\n]}\n" : "]}\n");
        }
    } else if (what) {
        const rp_msi_wtable_t *t = NULL;
        for (size_t i = 0; i < db->table_count; ++i) {
            if (strcmp(db->tables[i].name, what) == 0) t = &db->tables[i];
        }
        if (t == NULL) {
            rp_diag_error(RP_DIAG_NO_TABLE, "no table '%s' in '%s'", what, path);
            rc = RP_EXIT_USAGE;
        } else {
            table_json(&b, t);
            rp_buf_byte(&b, '\n');
        }
    } else {
        rp_buf_puts(&b, "{\"codepage\": ");
        rp_buf_long(&b, db->codepage);
        rp_buf_puts(&b, ", \"tables\": [\n");
        for (size_t i = 0; i < db->table_count; ++i) {
            if (i) rp_buf_puts(&b, ",\n");
            table_json(&b, &db->tables[i]);
        }
        rp_buf_puts(&b, "\n]}\n");
    }
    if (rc == RP_EXIT_OK) {
        if (b.err != PROVEN_OK || rp_pal_write(RP_OUT_STDOUT, b.data, b.len) != PROVEN_OK) rc = RP_EXIT_IO;
    }
    rp_buf_free(&b);
    rp_pkg_close(heap, &p);
    return rc;
}
