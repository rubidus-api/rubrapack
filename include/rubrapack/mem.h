#ifndef RUBRAPACK_MEM_H
#define RUBRAPACK_MEM_H

// include/rubrapack/mem.h - checked array allocation through a proven allocator.

#include <stdckdint.h>
#include <stddef.h>

#include "proven/allocator.h"

// Allocates count * size bytes (at least 1) aligned for max_align_t; NULL on overflow or failure.
[[nodiscard]] static inline void *rp_mem_alloc(proven_allocator_t alloc, size_t count, size_t size) {
    size_t bytes;
    if (ckd_mul(&bytes, count, size)) return NULL;
    proven_result_mem_mut_t r = alloc.alloc_fn(alloc.ctx, bytes == 0 ? 1 : bytes, alignof(max_align_t));
    return r.err == PROVEN_OK ? r.value.ptr : NULL;
}

static inline void rp_mem_free(proven_allocator_t alloc, void *p) {
    if (p != NULL) alloc.free_fn(alloc.ctx, p);
}

#endif // RUBRAPACK_MEM_H
