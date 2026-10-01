#ifndef RUBRAPACK_MEM_H
#define RUBRAPACK_MEM_H

// include/rubrapack/mem.h - checked array allocation through a proven allocator, and sorting
// and searching a plain C array with proven's algorithms.

#include <stdckdint.h>
#include <stddef.h>

#include "proven/algorithm.h"
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

// proven's array view of count elements of size bytes at base (nothing is allocated through it).
static inline proven_array_t rp_array_view(void *base, size_t count, size_t size) {
    return (proven_array_t){ .data = base, .len = count, .cap = count, .elem_size = size, .align = alignof(max_align_t) };
}

// Sorts in place (proven_array_sort: introsort, not stable, so cmp must order every pair it
// can be given - break ties).
static inline void rp_sort(void *base, size_t count, size_t size, proven_compare_fn_t cmp) {
    proven_array_t a = rp_array_view(base, count, size);
    proven_array_sort(&a, cmp);
}

// Binary search of a sorted array: the element equal to key, or NULL.
static inline void *rp_bsearch(const void *key, const void *base, size_t count, size_t size, proven_compare_fn_t cmp) {
    proven_array_t a = rp_array_view((void *)base, count, size);
    return proven_array_binary_search(&a, key, cmp);
}

#endif // RUBRAPACK_MEM_H
