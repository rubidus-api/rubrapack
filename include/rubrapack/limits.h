#ifndef RUBRAPACK_LIMITS_H
#define RUBRAPACK_LIMITS_H

// include/rubrapack/limits.h
//
// Resource limits shared by every reader, writer, and extractor (RFC-0001 section 14.3).
// A format module never truncates silently: it charges what it reads or writes against a
// budget, and the first charge that would cross a limit fails with PROVEN_ERR_OUT_OF_BOUNDS.
// Size arithmetic goes through the checked helpers so an overflow is an error, not a wrap.

#include <stddef.h>
#include <stdint.h>

#include "proven/types.h"

typedef struct {
    uint64_t max_entries;       // files/streams/records in one container
    uint64_t max_metadata;      // bytes of tables, directories, manifests, string pools
    uint64_t max_output;        // bytes written by one extract (actual bytes, counted)
    uint32_t max_depth;         // nesting of structures (storages, XML elements, ...)
} rp_limits_t;

// Default limits: 100,000 entries, 256 MiB metadata, 16 GiB extracted, depth 64.
[[nodiscard]] rp_limits_t rp_limits_default(void);

// A running total against one limit.
typedef struct {
    uint64_t used;
    uint64_t limit;
} rp_budget_t;

[[nodiscard]] static inline rp_budget_t rp_budget(uint64_t limit) {
    return (rp_budget_t){ .used = 0, .limit = limit };
}

// Adds `amount` to the budget. On PROVEN_ERR_OUT_OF_BOUNDS or PROVEN_ERR_OVERFLOW the budget
// is unchanged.
[[nodiscard]] proven_err_t rp_budget_charge(rp_budget_t *budget, uint64_t amount);

// Checked size arithmetic: *out is written only on PROVEN_OK.
[[nodiscard]] proven_err_t rp_size_add(size_t *out, size_t a, size_t b);
[[nodiscard]] proven_err_t rp_size_mul(size_t *out, size_t a, size_t b);

// True when [offset, offset + size) lies inside [0, total), with no overflow.
[[nodiscard]] bool rp_range_ok(uint64_t offset, uint64_t size, uint64_t total);

#endif // RUBRAPACK_LIMITS_H
