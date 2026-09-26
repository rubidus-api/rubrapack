#ifndef RUBRAPACK_SUMINFO_H
#define RUBRAPACK_SUMINFO_H

// include/rubrapack/suminfo.h - the MSI summary information property set (MS-OLEPS; format
// notes F3): reader and IDT export.

#include <stddef.h>
#include <stdint.h>

#include "proven/allocator.h"
#include "proven/types.h"

enum {
    RP_VT_I2 = 2,
    RP_VT_I4 = 3,
    RP_VT_LPSTR = 30,
    RP_VT_FILETIME = 64,
};

enum { RP_SUMINFO_MAX = 32 };

typedef struct {
    uint32_t       pid;
    uint16_t       type;        // RP_VT_*
    int32_t        i;           // VT_I2 / VT_I4
    uint64_t       filetime;    // VT_FILETIME
    const uint8_t *str;         // VT_LPSTR: bytes without the NUL, in the set's code page
    size_t         str_len;
} rp_suminfo_prop_t;

typedef struct {
    rp_suminfo_prop_t props[RP_SUMINFO_MAX];    // in stream order
    size_t            count;
} rp_suminfo_t;

// Parses the `\005SummaryInformation` stream; string pointers point into `data`.
[[nodiscard]] proven_err_t rp_suminfo_parse(const uint8_t *data, size_t len, rp_suminfo_t *out);

// `_SummaryInformation` pseudo-table as MsiDatabaseExport writes it.
[[nodiscard]] proven_err_t rp_suminfo_export_idt(const rp_suminfo_t *si, proven_allocator_t alloc, uint8_t **out,
                                                 size_t *len);

#endif // RUBRAPACK_SUMINFO_H
