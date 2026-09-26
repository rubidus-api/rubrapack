#ifndef RUBRAPACK_CAB_H
#define RUBRAPACK_CAB_H

// include/rubrapack/cab.h - cabinet files (MS-CAB; RFC-0001 F4): one folder, stored or MSZIP
// (MS-MCI: each 32 KiB block is "CK" + a raw deflate stream).

#include <stddef.h>
#include <stdint.h>

#include "proven/allocator.h"
#include "proven/types.h"
#include "rubrapack/limits.h"

typedef struct {
    const char    *name;    // ASCII (MSI: the File table key)
    const uint8_t *data;
    size_t         size;
} rp_cab_file_t;

// Writes one cabinet with a single folder: compress -1 = stored, 0-9 = MSZIP at that deflate
// level (each block compressed on its own). Deterministic: every file gets the date 1980-01-01
// 00:00 and the archive attribute. Files are stored in the order given.
[[nodiscard]] proven_err_t rp_cab_write(proven_allocator_t alloc, const rp_cab_file_t *files, size_t count, int compress,
                                        const rp_limits_t *limits, uint8_t **out, size_t *len);

// Reads a cabinet with stored or MSZIP folders (MSZIP blocks may use the previous blocks of the
// folder as their dictionary): checks the header, block checksums and bounds, and returns
// every file. Names point into `cab`; data points into *arena. Free *files and *arena with
// rp_mem_free.
[[nodiscard]] proven_err_t rp_cab_read(proven_allocator_t alloc, const uint8_t *cab, size_t len,
                                       const rp_limits_t *limits, rp_cab_file_t **files, size_t *count,
                                       uint8_t **arena);

#endif // RUBRAPACK_CAB_H
