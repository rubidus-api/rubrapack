#ifndef RUBRAPACK_TRANSFORM_H
#define RUBRAPACK_TRANSFORM_H

// include/rubrapack/transform.h - Windows Installer transforms (.mst; RFC-0016 3): the difference
// that turns a base database into a target one, in the layout msi.dll's MsiDatabaseGenerateTransform
// writes (format notes F2 "Transforms"), and a listing of one against its base.

#include <stddef.h>
#include <stdint.h>

#include "proven/allocator.h"
#include "proven/types.h"
#include "rubrapack/buf.h"
#include "rubrapack/cfb.h"
#include "rubrapack/limits.h"
#include "rubrapack/msi.h"

// Summary property 16 of a transform: the low word says what msiexec checks before it applies the
// transform, the high word which errors it ignores while applying.
enum {
    RP_MST_VALIDATE_LANGUAGE = 0x0001,
    RP_MST_VALIDATE_PRODUCT = 0x0002,
    RP_MST_VALIDATE_PLATFORM = 0x0004,
    RP_MST_VALIDATE_UPGRADE_CODE = 0x0800,
};

typedef struct {
    const rp_msi_wdb_t *db;     // tables and summary (rp_msi_view)
    const rp_cfb_t     *cfb;    // the package, for the streams of binary cells
} rp_mst_side_t;

// Writes the transform from base to target. A table whose columns differ, a changed File or Media
// table (a transform carries no files), or a changed column past the 16th is refused: the result is
// PROVEN_ERR_UNSUPPORTED and *why says which. Deterministic.
[[nodiscard]] proven_err_t rp_mst_write(proven_allocator_t alloc, const rp_mst_side_t *base, const rp_mst_side_t *target,
                                        uint32_t flags, const rp_limits_t *limits, uint8_t **out, size_t *len,
                                        const char **why, char *table, size_t table_cap);

// Lists a transform's records against its base, one per line in stored order:
// "<Table>\tinsert\t<cells>", "<Table>\tupdate\t<keys>\t<Column>=<value>...", "<Table>\tdelete\t<keys>".
// A string is written as in an IDT file, a binary cell as "[stream <Table>.<keys>, <n> bytes]".
[[nodiscard]] proven_err_t rp_mst_list(proven_allocator_t alloc, const rp_cfb_t *mst, const rp_msi_wdb_t *base,
                                       const rp_limits_t *limits, rp_buf_t *out, const char **why);

#endif // RUBRAPACK_TRANSFORM_H
