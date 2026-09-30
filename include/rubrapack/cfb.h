#ifndef RUBRAPACK_CFB_H
#define RUBRAPACK_CFB_H

// include/rubrapack/cfb.h - compound file (MS-CFB) reader over a file held in memory.
//
// Versions 3 (512-byte sectors) and 4 (4096-byte sectors). Every chain, sector number, stream
// size and directory link is checked before use; a file that loops, points outside itself, or
// exceeds the limits is refused, never followed (RFC-0001 section 14.3).

#include <stddef.h>
#include <stdint.h>

#include "proven/allocator.h"
#include "proven/types.h"
#include "rubrapack/limits.h"

#define RP_CFB_NONE UINT32_C(0xFFFFFFFF)

enum {
    RP_CFB_UNUSED = 0,
    RP_CFB_STORAGE = 1,
    RP_CFB_STREAM = 2,
    RP_CFB_ROOT = 5,
};

typedef struct {
    uint16_t name[32];      // UTF-16 units, NUL-terminated
    uint32_t name_len;      // units, without the NUL
    uint8_t  type;          // RP_CFB_*
    uint32_t left, right, child;
    uint8_t  clsid[16];
    uint32_t state;         // state bits
    uint8_t  times[16];     // creation and modification FILETIMEs, as stored
    uint32_t start;         // first sector (regular or mini)
    uint64_t size;
} rp_cfb_entry_t;

typedef struct {
    proven_allocator_t alloc;
    const uint8_t     *data;        // borrowed: the whole file
    size_t             len;
    uint16_t           major;       // 3 or 4
    uint32_t           sector_size; // 512 or 4096
    uint32_t           sector_count;
    uint32_t          *fat;
    size_t             fat_len;
    uint32_t          *minifat;
    size_t             minifat_len;
    uint8_t           *mini;        // the mini stream (root entry's data)
    size_t             mini_len;
    rp_cfb_entry_t    *entries;
    size_t             entry_count;
    uint32_t           mini_cutoff;
} rp_cfb_t;

// Parses the compound file in data[0..len). `data` must outlive the handle. On failure *why
// (when not NULL) names the first problem found.
[[nodiscard]] proven_err_t rp_cfb_open(rp_cfb_t *cfb, proven_allocator_t alloc, const uint8_t *data,
                                       size_t len, const rp_limits_t *limits, const char **why);
void rp_cfb_close(rp_cfb_t *cfb);

// Lists the children of a storage (or the root, id 0) in directory-tree order. With ids ==
// NULL only counts. PROVEN_ERR_INVALID_FORMAT when the tree loops.
[[nodiscard]] proven_err_t rp_cfb_children(const rp_cfb_t *cfb, uint32_t storage, uint32_t *ids,
                                           size_t cap, size_t *count);

// Finds a child of `storage` by exact UTF-16 name.
[[nodiscard]] proven_err_t rp_cfb_find(const rp_cfb_t *cfb, uint32_t storage, const uint16_t *name,
                                       size_t name_len, uint32_t *id);

// Copies a whole stream into dst (cap >= entries[id].size).
[[nodiscard]] proven_err_t rp_cfb_read(const rp_cfb_t *cfb, uint32_t id, uint8_t *dst, size_t cap);

// ---- writer ----------------------------------------------------------------------------------

typedef struct {
    const uint16_t *name;       // UTF-16, at most 31 units
    size_t          name_len;
    const uint8_t  *data;
    size_t          size;
    size_t          parent;     // 0: the root; k + 1: inside streams[k], which is a storage
    bool            storage;    // a storage (no data) that other entries name as parent
    const uint8_t  *clsid;      // a storage's class (16 bytes), or NULL
} rp_cfb_stream_t;

// Writes a compound file holding `streams` (and storages; `parent`) with the root CLSID given.
// sector_shift 9 = version 3 (512-byte sectors), 12 = version 4 (4096). Deterministic: all times
// are zero and the directory tree depends only on the names. Names must be distinct.
[[nodiscard]] proven_err_t rp_cfb_write(proven_allocator_t alloc, unsigned sector_shift, const uint8_t clsid[16],
                                        const rp_cfb_stream_t *streams, size_t count, const rp_limits_t *limits,
                                        uint8_t **out, size_t *len);

// Where a writer puts the finished file (RFC-0013 R2b): get hands out a zeroed buffer of `len`
// bytes, such as a mapped output file; drop takes it back when the write fails. Without a sink the
// buffer is heap memory, freed with rp_mem_free.
typedef struct {
    proven_err_t (*get)(void *ctx, size_t len, uint8_t **data);
    void (*drop)(void *ctx, uint8_t *data);
    void *ctx;
} rp_out_sink_t;

// rp_cfb_write into the sink's buffer (NULL: the heap, as rp_cfb_write).
[[nodiscard]] proven_err_t rp_cfb_write_to(proven_allocator_t alloc, unsigned sector_shift, const uint8_t clsid[16],
                                           const rp_cfb_stream_t *streams, size_t count, const rp_limits_t *limits,
                                           const rp_out_sink_t *sink, uint8_t **out, size_t *len);

#endif // RUBRAPACK_CFB_H
