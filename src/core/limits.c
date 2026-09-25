// src/core/limits.c - resource limits and checked size arithmetic (include/rubrapack/limits.h).

#include "rubrapack/limits.h"

#include <stdckdint.h>

rp_limits_t rp_limits_default(void) {
    return (rp_limits_t){
        .max_entries = 100000,
        .max_metadata = UINT64_C(256) << 20,
        .max_output = UINT64_C(16) << 30,
        .max_depth = 64,
    };
}

proven_err_t rp_budget_charge(rp_budget_t *budget, uint64_t amount) {
    if (budget == NULL) return PROVEN_ERR_INVALID_ARG;
    uint64_t next;
    if (ckd_add(&next, budget->used, amount)) return PROVEN_ERR_OVERFLOW;
    if (next > budget->limit) return PROVEN_ERR_OUT_OF_BOUNDS;
    budget->used = next;
    return PROVEN_OK;
}

proven_err_t rp_size_add(size_t *out, size_t a, size_t b) {
    size_t r;
    if (out == NULL) return PROVEN_ERR_INVALID_ARG;
    if (ckd_add(&r, a, b)) return PROVEN_ERR_OVERFLOW;
    *out = r;
    return PROVEN_OK;
}

proven_err_t rp_size_mul(size_t *out, size_t a, size_t b) {
    size_t r;
    if (out == NULL) return PROVEN_ERR_INVALID_ARG;
    if (ckd_mul(&r, a, b)) return PROVEN_ERR_OVERFLOW;
    *out = r;
    return PROVEN_OK;
}

bool rp_range_ok(uint64_t offset, uint64_t size, uint64_t total) {
    uint64_t end;
    if (ckd_add(&end, offset, size)) return false;
    return end <= total;
}
