#ifndef RUBRAPACK_CAB_H
#define RUBRAPACK_CAB_H

// include/rubrapack/cab.h - cabinet files (MS-CAB; RFC-0001 F4): stored or MSZIP folders (MS-MCI:
// each 32 KiB block is "CK" + a raw deflate stream that may refer to the folder's earlier blocks).

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

// LZX with a window of 2^w bytes (15 <= w <= 21), as a `compress` value.
#define RP_CAB_LZX(w) (100 + (w))

// Writes one cabinet: compress -1 = stored, 0-9 = MSZIP at that deflate level (each block using the
// previous block of its folder as its dictionary), RP_CAB_LZX(w) = LZX (one thread per cabinet;
// include/rubrapack/lzx.h). A new folder starts (at a file boundary) before a
// folder would pass 65,535 blocks. Deterministic, whatever `jobs` (threads compressing blocks):
// every file gets the date 1980-01-01 00:00 and the archive attribute; files in the order given.
[[nodiscard]] proven_err_t rp_cab_write_ex(proven_allocator_t alloc, const rp_cab_file_t *files, size_t count, int compress,
                                           size_t jobs, const rp_limits_t *limits, uint8_t **out, size_t *len);
// Blocks per folder at most (0xFFFF, the format's limit). Tests lower it to make several folders
// from a small package (RP_TEST_CAB_FOLDER_BLOCKS in the build).
extern size_t rp_cab_folder_blocks;

// The same on one thread.
[[nodiscard]] proven_err_t rp_cab_write(proven_allocator_t alloc, const rp_cab_file_t *files, size_t count, int compress,
                                        const rp_limits_t *limits, uint8_t **out, size_t *len);

// Reads a cabinet with stored, MSZIP or LZX folders (MSZIP blocks may use the previous blocks of the
// folder as their dictionary): checks the header, block checksums and bounds, and returns
// every file. Names point into `cab`; data points into *arena. Free *files and *arena with
// rp_mem_free.
[[nodiscard]] proven_err_t rp_cab_read(proven_allocator_t alloc, const uint8_t *cab, size_t len,
                                       const rp_limits_t *limits, rp_cab_file_t **files, size_t *count,
                                       uint8_t **arena);

#endif // RUBRAPACK_CAB_H
