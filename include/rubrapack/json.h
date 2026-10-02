#ifndef RUBRAPACK_JSON_H
#define RUBRAPACK_JSON_H

// include/rubrapack/json.h - `--json` output (RFC-0025): a JSON string, and the MSI tables and
// summary of `inspect --json`. lint's JSON goes through the source diagnostics (srcdiag.h).

#include <stddef.h>
#include <stdint.h>

#include "rubrapack/buf.h"

// A JSON string of `n` bytes of UTF-8 (control characters, '"' and '\' escaped).
void rp_json_str(rp_buf_t *b, const char *s, size_t n);

// `inspect <file.msi> --json` (every table) and `inspect <file.msi> <table> --json`; `--summary --json`.
[[nodiscard]] int rp_inspect_json(const char *path, const char *what);

#endif // RUBRAPACK_JSON_H
