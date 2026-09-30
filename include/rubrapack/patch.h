#ifndef RUBRAPACK_PATCH_H
#define RUBRAPACK_PATCH_H

// include/rubrapack/patch.h - Windows Installer patches (.msp; RFC-0016 3): a small update or minor
// upgrade from one package to the next, with the changed and added files whole, in the layout
// Microsoft's patch creation tool (MsiMsp.exe / patchwiz.dll) writes (format notes F2 "Patches").

#include <stddef.h>
#include <stdint.h>

#include "proven/allocator.h"
#include "proven/types.h"
#include "rubrapack/limits.h"
#include "rubrapack/transform.h"

typedef struct {
    const char *patch_code;         // "{GUID}"; NULL: derived from the two package codes
    const char *source_list;        // summary property 5: where the patch can be found again ("" when NULL)
    const char *family;             // MsiPatchSequence.PatchFamily; NULL: the product name
    bool        no_removal;         // MsiPatchMetadata AllowRemoval = 0
} rp_msp_opts_t;

// Writes the patch from base to target. What a patch cannot carry is refused with
// PROVEN_ERR_UNSUPPORTED and *why (and `table` naming what it is about): another product or upgrade
// code, another major or minor version or a lower one, a file or component the target no longer
// has, a component whose GUID changed, a cabinet that is not embedded. Deterministic.
[[nodiscard]] proven_err_t rp_msp_write(proven_allocator_t alloc, const rp_mst_side_t *base, const rp_mst_side_t *target,
                                        const rp_msp_opts_t *opts, const rp_limits_t *limits, uint8_t **out, size_t *len,
                                        const char **why, char *table, size_t table_cap);

#endif // RUBRAPACK_PATCH_H
