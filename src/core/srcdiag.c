// src/core/srcdiag.c - source-file diagnostics (include/rubrapack/srcdiag.h).

#include "rubrapack/pal.h"
#include "rubrapack/srcdiag.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void rp_srcdiag_add(rp_srcdiags_t *d, rp_pos_t pos, const char *code, bool warning, const char *fmt, ...) {
    if (!warning) d->errors++;
    if (d->count >= RP_SRCDIAG_MAX) {
        d->dropped++;
        return;
    }
    rp_srcdiag_t *e = &d->items[d->count++];
    e->pos = pos;
    e->warning = warning;
    snprintf(e->code, sizeof e->code, "%s", code);
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(e->msg, sizeof e->msg, fmt, ap);
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
    char line[1024];
    for (size_t i = 0; i < d->count; ++i) {
        const rp_srcdiag_t *e = &d->items[i];
        if (e->pos.line == 0) {         // about the output as a whole, not a source line
            snprintf(line, sizeof line, "%s: %s[%s]: %s\n", path, e->warning ? "warning" : "error", e->code, e->msg);
        } else {
            snprintf(line, sizeof line, "%s:%u:%u: %s[%s]: %s\n", path, (unsigned)e->pos.line, (unsigned)e->pos.col,
                     e->warning ? "warning" : "error", e->code, e->msg);
        }
        (void)rp_pal_puts(RP_OUT_STDERR, line);
    }
    if (d->dropped) {
        snprintf(line, sizeof line, "%s: %zu more diagnostics not shown\n", path, d->dropped);
        (void)rp_pal_puts(RP_OUT_STDERR, line);
    }
}
