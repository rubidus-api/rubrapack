// src/msi/view.c - a read database as writer tables (include/rubrapack/msi.h, "view").

#include "rubrapack/mem.h"
#include "rubrapack/msi.h"

#include <string.h>

typedef struct {
    rp_msi_wtable_t  *tables;
    rp_msi_wcolumn_t **columns;     // per table
    rp_msi_cell_t    **cells;       // per table
    char            **names;        // NUL-terminated copies of table and column names
    size_t            name_count, name_cap, table_count;
} priv_t;

static const char *name_copy(const rp_msi_t *msi, priv_t *p, uint32_t id) {
    const uint8_t *b;
    size_t n;
    if (rp_msi_string(msi, id, &b, &n) != PROVEN_OK) return NULL;
    if (p->name_count == p->name_cap) return NULL;
    char *s = rp_mem_alloc(msi->alloc, n + 1, 1);
    if (s == NULL) return NULL;
    memcpy(s, b, n);
    s[n] = '\0';
    p->names[p->name_count++] = s;
    return s;
}

void rp_msi_view_free(const rp_msi_t *msi, rp_msi_view_t *view) {
    priv_t *p = view->priv;
    if (p == NULL) return;
    for (size_t i = 0; i < p->table_count; ++i) {
        if (p->columns) rp_mem_free(msi->alloc, p->columns[i]);
        if (p->cells) rp_mem_free(msi->alloc, p->cells[i]);
    }
    for (size_t i = 0; i < p->name_count; ++i) rp_mem_free(msi->alloc, p->names[i]);
    rp_mem_free(msi->alloc, p->names);
    rp_mem_free(msi->alloc, p->columns);
    rp_mem_free(msi->alloc, p->cells);
    rp_mem_free(msi->alloc, p->tables);
    rp_mem_free(msi->alloc, p);
    view->priv = NULL;
}

proven_err_t rp_msi_view(const rp_msi_t *msi, const uint8_t *summary, size_t summary_len, rp_msi_view_t *view) {
    memset(view, 0, sizeof *view);
    size_t nt = msi->table_count, cap = nt;
    for (size_t t = 0; t < nt; ++t) cap += msi->tables[t].column_count;
    priv_t *p = rp_mem_alloc(msi->alloc, 1, sizeof *p);
    if (p == NULL) return PROVEN_ERR_NOMEM;
    memset(p, 0, sizeof *p);
    view->priv = p;
    p->tables = rp_mem_alloc(msi->alloc, nt + 1, sizeof *p->tables);
    p->columns = rp_mem_alloc(msi->alloc, nt + 1, sizeof *p->columns);
    p->cells = rp_mem_alloc(msi->alloc, nt + 1, sizeof *p->cells);
    p->names = rp_mem_alloc(msi->alloc, cap + 1, sizeof *p->names);
    p->name_cap = cap;
    if (p->tables == NULL || p->columns == NULL || p->cells == NULL || p->names == NULL) {
        rp_msi_view_free(msi, view);
        return PROVEN_ERR_NOMEM;
    }
    memset(p->columns, 0, (nt + 1) * sizeof *p->columns);
    memset(p->cells, 0, (nt + 1) * sizeof *p->cells);
    proven_err_t err = PROVEN_OK;
    for (size_t t = 0; t < nt && err == PROVEN_OK; ++t) {
        const rp_msi_table_t *mt = &msi->tables[t];
        p->table_count = t + 1;
        rp_msi_wtable_t *wt = &p->tables[t];
        memset(wt, 0, sizeof *wt);
        wt->name = name_copy(msi, p, mt->name);
        p->columns[t] = rp_mem_alloc(msi->alloc, mt->column_count + 1, sizeof **p->columns);
        if (wt->name == NULL || p->columns[t] == NULL) {
            err = PROVEN_ERR_NOMEM;
            break;
        }
        for (size_t c = 0; c < mt->column_count; ++c) {
            p->columns[t][c].name = name_copy(msi, p, mt->columns[c].name);
            p->columns[t][c].type = mt->columns[c].type;
            if (p->columns[t][c].name == NULL) err = PROVEN_ERR_NOMEM;
        }
        wt->columns = p->columns[t];
        wt->column_count = mt->column_count;
        rp_msi_rows_t rows;
        if (err == PROVEN_OK) err = rp_msi_read_rows(msi, t, &rows);
        if (err != PROVEN_OK) break;
        size_t n = rows.row_count * rows.column_count;
        p->cells[t] = rp_mem_alloc(msi->alloc, n + 1, sizeof **p->cells);
        if (p->cells[t] == NULL) err = PROVEN_ERR_NOMEM;
        for (size_t i = 0; err == PROVEN_OK && i < n; ++i) {
            const rp_msi_value_t *v = &rows.cells[i];
            rp_msi_cell_t *x = &p->cells[t][i];
            memset(x, 0, sizeof *x);
            x->kind = v->kind;
            if (v->kind == RP_MSI_INT) x->i = v->i;
            else if (v->kind == RP_MSI_STR && rp_msi_string(msi, v->s, &x->bytes, &x->len) != PROVEN_OK) err = PROVEN_ERR_INVALID_FORMAT;
        }
        wt->cells = p->cells[t];
        wt->row_count = rows.row_count;
        rp_msi_rows_free(msi, &rows);
    }
    if (err != PROVEN_OK) {
        rp_msi_view_free(msi, view);
        return err;
    }
    view->db = (rp_msi_wdb_t){ .codepage = msi->codepage, .tables = p->tables, .table_count = nt,
                               .summary = summary, .summary_len = summary_len };
    return PROVEN_OK;
}
