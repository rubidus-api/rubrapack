#ifndef RUBRAPACK_BUILD_H
#define RUBRAPACK_BUILD_H

// include/rubrapack/build.h - IR -> Windows Installer package (RFC-0001 section 9, P2 scope:
// machine scope, files, folders, features, upgrade/downgrade, stored cabinet).

#include <stddef.h>
#include <stdint.h>

#include "proven/allocator.h"
#include "proven/types.h"
#include "rubrapack/ir.h"
#include "rubrapack/limits.h"
#include "rubrapack/srcdiag.h"

typedef struct {
    bool reproducible;      // package code from the content instead of random (RFC-0001 14.2)
} rp_build_options_t;

// Reads every source file, then writes the MSI bytes to *out (free with rp_mem_free).
// Problems found while lowering (unreadable file, too many files, ...) go to `diags`.
[[nodiscard]] proven_err_t rp_msi_from_ir(proven_allocator_t alloc, const rp_ir_t *ir, const rp_build_options_t *opt,
                                          const rp_limits_t *limits, uint8_t **out, size_t *len, rp_srcdiags_t *diags);

#endif // RUBRAPACK_BUILD_H
