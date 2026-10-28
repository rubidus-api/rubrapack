// src/cli/schema.c - `rubrapack schema` (RFC-0025): the JSON Schema of rubrapack sources, for an
// editor that completes and checks the keys while you type (schema_table.c, generated from the
// parser's key lists).

#include "rubrapack/cli.h"
#include "rubrapack/diag.h"
#include "rubrapack/inspect.h"
#include "rubrapack/pal.h"

#include <string.h>

extern const char *const rp_schema_lines[];
extern const size_t rp_schema_line_count;

int rp_cmd_schema(int argc, char **argv) {
    (void)argv;
    if (argc != 2) {
        (void)rp_cli_usage_error("schema");
        return RP_EXIT_USAGE;
    }
    for (size_t i = 0; i < rp_schema_line_count; ++i) {
        if (rp_pal_puts(RP_OUT_STDOUT, rp_schema_lines[i]) != PROVEN_OK) return RP_EXIT_IO;
    }
    return RP_EXIT_OK;
}
