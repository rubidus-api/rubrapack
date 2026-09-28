#ifndef RUBRAPACK_ZIP_H
#define RUBRAPACK_ZIP_H

// include/rubrapack/zip.h - ZIP archives (PKWARE APPNOTE 6.3) as MSIX packages use them (RFC-0009;
// what Windows' packaging API writes, docs/research/2026-09-27-p8a-msix-oracle.md): every entry
// ZIP64, local headers without sizes followed by ZIP64 data descriptors, the central directory with
// ZIP64 extras, and a ZIP64 end record. Methods: stored (0) and deflate (8). Dates are fixed at
// 1980-01-01 00:00, so the same entries give the same bytes (RFC-0006).

#include <stddef.h>
#include <stdint.h>

#include "proven/allocator.h"
#include "proven/types.h"
#include "rubrapack/buf.h"
#include "rubrapack/limits.h"

// CRC-32 (ISO 3309, as ZIP and PNG use it): crc = rp_crc32(0, data, n), or chained over parts.
uint32_t rp_crc32(uint32_t crc, const void *data, size_t n);

typedef struct {
    rp_buf_t  out;
    rp_buf_t  central;
    uint64_t  count;
    bool      signer_form;  // as Windows' signer rewrites a package (RFC-0011): central records in ZIP32
                            // form when they fit, and the end record's disk numbers 0 (else 0xFFFF)
} rp_zip_writer_t;

void rp_zip_begin(rp_zip_writer_t *w, proven_allocator_t alloc, size_t limit);
// Adds an entry: `name` as stored (an OPC part name: ASCII, percent-encoded), `data` as stored
// (compressed for method 8). *lfh_size gets the local header's size (30 + the name's length).
void rp_zip_add(rp_zip_writer_t *w, const char *name, int method, const uint8_t *data, size_t data_len, uint32_t crc, uint64_t size,
                size_t *lfh_size);
// Adds a stored or deflated entry the ZIP32 way: sizes in the local header, no data descriptor, version
// needed 2.0 (how Windows' signer writes AppxSignature.p7x). Under 4 GiB only.
void rp_zip_add_plain(rp_zip_writer_t *w, const char *name, int method, const uint8_t *data, size_t data_len, uint32_t crc, uint64_t size);
// Writes the central directory and the end records; the archive goes to *out (free with rp_mem_free).
[[nodiscard]] proven_err_t rp_zip_finish(rp_zip_writer_t *w, uint8_t **out, size_t *len);
void rp_zip_abort(rp_zip_writer_t *w);
// Appends to `out` what rp_zip_finish would write now: the central directory and the end records
// (for signing: the AppX SIP's AXCD, RFC-0011).
void rp_zip_tail(const rp_zip_writer_t *w, rp_buf_t *out);
// Appends to `out` the archive's central directory, as it is, without the entry `skip`, then the
// file's end records with counts, sizes and offsets as if that entry had never been written - the
// directory starting at `skip_lfh_off`, where the entry's local header is (for checking the AXCD
// of a signed package). The skipped entry must be described the ZIP32 way (Windows reads its place
// from those fields).
[[nodiscard]] bool rp_zip_central_without(const uint8_t *zip, size_t len, const char *skip, uint64_t skip_lfh_off, rp_buf_t *out,
                                          const char **why);

typedef struct {
    char    *name;              // as stored, NUL-terminated
    int      method;
    uint32_t crc;
    uint64_t csize, size;
    uint64_t data_off;          // where the stored bytes start
    uint64_t lfh_off;
    size_t   lfh_size;          // 30 + name + extra of the local header
} rp_zip_entry_t;

// Reads the central directory (ZIP64 or not). Every entry must lie inside the archive, its local
// header must agree with the central one on name and method, and entries must not overlap; at
// most lim->max_entries. Entries go to *entries (free with rp_zip_entries_free).
[[nodiscard]] proven_err_t rp_zip_read(proven_allocator_t alloc, const uint8_t *zip, size_t len, const rp_limits_t *lim,
                                       rp_zip_entry_t **entries, size_t *count, const char **why);
void rp_zip_entries_free(proven_allocator_t alloc, rp_zip_entry_t *entries, size_t count);

// The entry's plain bytes (inflated for method 8), checked against its size and CRC-32; at most
// `max` bytes. *out is freed with rp_mem_free.
[[nodiscard]] proven_err_t rp_zip_data(proven_allocator_t alloc, const uint8_t *zip, size_t len, const rp_zip_entry_t *e, uint64_t max,
                                       uint8_t **out, const char **why);

#endif // RUBRAPACK_ZIP_H
