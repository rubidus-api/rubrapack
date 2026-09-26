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

// `_SummaryInformation` pseudo-table as MsiDatabaseExport writes it, except that dates are
// printed in UTC (msi.dll prints the exporting machine's local time; format notes F3).
[[nodiscard]] proven_err_t rp_suminfo_export_idt(const rp_suminfo_t *si, proven_allocator_t alloc, uint8_t **out,
                                                 size_t *len);

// A FILETIME (100 ns since 1601-01-01 UTC) as "yyyy/mm/dd hh:mm:ss" in UTC, NUL-terminated.
// Years outside 0..9999 are not expected in a summary stream.
void rp_filetime_text(uint64_t ft, char out[20]);

// Writes a summary information stream from props (in the order given; callers pass them in
// property id order). VT_LPSTR bytes are written as given plus a terminator: one zero byte, or
// two when utf16 is true (code page 1200, MS-OLEPS CodePageString: Size counts bytes including
// the terminator). Values are padded to 4 bytes. Deterministic.
[[nodiscard]] proven_err_t rp_suminfo_write(const rp_suminfo_t *si, bool utf16, proven_allocator_t alloc, uint8_t **out,
                                            size_t *len);

#endif // RUBRAPACK_SUMINFO_H
