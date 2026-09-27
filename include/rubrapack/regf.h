#ifndef RUBRAPACK_REGF_H
#define RUBRAPACK_REGF_H

// include/rubrapack/regf.h - registry hive files (REGF) for MSIX packages' Registry.dat (RFC-0010;
// RFC-0001 F12). The format facts come from hives written by Windows' Offline Registry Library
// (offreg.dll, the writer of MSIX Registry.dat files) and read back by it and by RegLoadAppKey
// (tests/fixtures/regf, docs/research/2026-09-27-p8b-regf-oracle.md):
//   - a 4096-byte base block, then hive bins of 4096 bytes or a multiple; cells 8-byte aligned,
//     negative sizes for cells in use;
//   - key (nk), value (vk), subkey list (lh, split through ri beyond 507 entries), value list, one
//     shared security descriptor (sk), big data (db) beyond 16344 bytes of value data;
//   - names stored as Latin-1 when every UTF-16 unit is below 256 (the "compressed" flag), else
//     UTF-16LE; value data of up to 4 bytes kept in the value cell itself.
// rubrapack writes every time field as zero and lays cells out depth first, so the same keys and
// values give the same bytes on every host.

#include <stddef.h>
#include <stdint.h>

#include "proven/allocator.h"
#include "proven/types.h"
#include "rubrapack/limits.h"

typedef struct rp_regf rp_regf_t;

[[nodiscard]] rp_regf_t *rp_regf_new(proven_allocator_t alloc);
void rp_regf_free(rp_regf_t *h);

// Creates the key `path` (UTF-8, '\' between names, relative to the hive's root) and its parents;
// with `value` (UTF-8, "" for the default value) also sets that value. Names compare without case
// (Unicode simple upper case); the first spelling stays. PROVEN_ERR_INVALID_ARG for an empty name
// in the path, a key name over 255 UTF-16 units, a value name over 16383 or data over 16 MiB.
[[nodiscard]] proven_err_t rp_regf_set(rp_regf_t *h, const char *path, const char *value, uint32_t type, const uint8_t *data, size_t len);

// The hive file (free with rp_mem_free).
[[nodiscard]] proven_err_t rp_regf_write(rp_regf_t *h, uint8_t **out, size_t *len);

// Reads a hive and calls `key` for every key (path from the root, UTF-8, '\' separated, "" for the
// root) and `value` for every value, depth first, subkeys in stored order. Every cell must lie in
// its bin, lists must hold keys and values, the base block's checksum must match; a hive that was
// not closed cleanly (sequence numbers differ) is refused. At most lim->max_entries keys and values.
typedef struct {
    void (*key)(void *ctx, const char *path);
    void (*value)(void *ctx, const char *path, const char *name, uint32_t type, const uint8_t *data, size_t len);
    void *ctx;
} rp_regf_visitor_t;
[[nodiscard]] proven_err_t rp_regf_read(proven_allocator_t alloc, const uint8_t *hive, size_t len, const rp_limits_t *lim,
                                        const rp_regf_visitor_t *v, const char **why);

// Unicode simple upper case of one UTF-16 unit (src/text/upcase_tables.c).
uint16_t rp_upcase16(uint16_t c);

#endif // RUBRAPACK_REGF_H
