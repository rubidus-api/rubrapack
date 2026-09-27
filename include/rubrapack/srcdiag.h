#ifndef RUBRAPACK_SRCDIAG_H
#define RUBRAPACK_SRCDIAG_H

// include/rubrapack/srcdiag.h - diagnostics about a source file (RFC-0002 section 10):
// `path:line:col: error[RP1xxx]: text`, collected up to a limit per stage.

#include <stddef.h>
#include <stdint.h>

#include "rubrapack/diag.h"

typedef struct {
    uint32_t line;      // 1-based
    uint32_t col;       // 1-based, in Unicode code points
} rp_pos_t;

enum { RP_SRCDIAG_MAX = 20 };

typedef struct {
    rp_pos_t pos;
    char     code[8];
    char     msg[240];
    bool     warning;
} rp_srcdiag_t;

typedef struct {
    rp_srcdiag_t items[RP_SRCDIAG_MAX];
    size_t       count;
    size_t       errors;        // errors seen, including dropped ones
    size_t       warnings;      // warnings seen, including dropped ones
    size_t       dropped;
} rp_srcdiags_t;

[[gnu::format(RP_PRINTF_FORMAT, 5, 6)]]
void rp_srcdiag_add(rp_srcdiags_t *d, rp_pos_t pos, const char *code, bool warning, const char *fmt, ...);

// Prints every collected diagnostic to stderr, prefixed with `path`.
void rp_srcdiag_print(const rp_srcdiags_t *d, const char *path);

#endif // RUBRAPACK_SRCDIAG_H
