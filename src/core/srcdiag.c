// src/core/srcdiag.c - source-file diagnostics (include/rubrapack/srcdiag.h).

#include "rubrapack/pal.h"
#include "rubrapack/srcdiag.h"
#include "rubrapack/lang.h"
#include "rubrapack/str.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void rp_srcdiag_add(rp_srcdiags_t *d, rp_pos_t pos, const char *code, bool warning, const char *fmt, ...) {
    if (!warning) d->errors++;
    else d->warnings++;
    if (d->count >= RP_SRCDIAG_MAX) {
        d->dropped++;
        return;
    }
    rp_srcdiag_t *e = &d->items[d->count++];
    e->pos = pos;
    e->warning = warning;
    (void)proven_fmt_to(e->code, sizeof e->code, "{}", proven_arg_cstr(code));
    va_list ap;
    va_start(ap, fmt);
    int n = rp_lang_vformat(e->msg, sizeof e->msg, fmt, ap);
    va_end(ap);
    if (n >= (int)sizeof e->msg) {     // cut: drop a partial UTF-8 character at the end
        size_t len = sizeof e->msg - 1, p = len;
        while (p > 0 && ((unsigned char)e->msg[p - 1] & 0xC0u) == 0x80u) --p;
        if (p > 0) {
            unsigned char lead = (unsigned char)e->msg[p - 1];
            size_t need = lead < 0x80 ? 1 : lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : 2;
            if ((p - 1) + need > len) e->msg[p - 1] = '\0';
        }
    }
}

void rp_srcdiag_print(const rp_srcdiags_t *d, const char *path) {
    if (rp_diag_json_on()) {        // `lint --json`: collected, printed at the end as one object
        for (size_t i = 0; i < d->count; ++i) {
            const rp_srcdiag_t *e = &d->items[i];
            rp_diag_json_add(path, e->pos.line, e->pos.col, e->code, e->warning, e->msg);
        }
        rp_diag_json_not_shown(d->dropped);
        return;
    }
    char line[1024];
    for (size_t i = 0; i < d->count; ++i) {
        const rp_srcdiag_t *e = &d->items[i];
        if (e->pos.line == 0) {         // about the output as a whole, not a source line
            (void)proven_fmt_to(line, sizeof line, "{}: {}[{}]: {}\n", proven_arg_cstr(path), proven_arg_cstr(rp_tr(e->warning ? "warning" : "error")), proven_arg_cstr(e->code), proven_arg_cstr(e->msg));
        } else {
            (void)proven_fmt_to(line, sizeof line, "{}:{}:{}: {}[{}]: {}\n", proven_arg_cstr(path), proven_arg_u32((unsigned)e->pos.line), proven_arg_u32((unsigned)e->pos.col), proven_arg_cstr(rp_tr(e->warning ? "warning" : "error")), proven_arg_cstr(e->code), proven_arg_cstr(e->msg));
        }
        (void)rp_pal_puts(RP_OUT_STDERR, line);
    }
    if (d->dropped) {
        rp_lang_format(line, sizeof line, "%s: %zu more diagnostics not shown\n", path, d->dropped);
        (void)rp_pal_puts(RP_OUT_STDERR, line);
    }
}
