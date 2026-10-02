// src/cli/explain_int.h - the table of `rubrapack explain` (explain.c), generated into
// explain_table.c from the messages in src/ (RFC-0025). Not a public header.

#ifndef RUBRAPACK_EXPLAIN_INT_H
#define RUBRAPACK_EXPLAIN_INT_H

#include <stddef.h>

typedef struct {
    const char *code;           // "RP1612"
    size_t      count;          // messages below (at most 8)
    const char *messages[8];    // the format strings, conversions shown as *
} rp_explain_entry_t;

extern const rp_explain_entry_t rp_explain_table[];
extern const size_t rp_explain_count;

#endif // RUBRAPACK_EXPLAIN_INT_H
