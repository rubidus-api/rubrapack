#ifndef RUBRAPACK_DEFLATE_H
#define RUBRAPACK_DEFLATE_H

// include/rubrapack/deflate.h - raw deflate (RFC 1951) encoder and decoder. Used for MSZIP
// cabinet blocks (MS-MCI), MSIX blocks and PNG (RFC-0001 F5).

#include <stddef.h>
#include <stdint.h>

#include "proven/allocator.h"
#include "proven/types.h"

// Compresses in[0..n) into one complete raw deflate stream (last block marked final).
// level 0 = stored blocks only; 1-9 = LZ77 with longer searches as the level rises, each block
// written as fixed Huffman, dynamic Huffman or stored, whichever is shortest. Deterministic.
// No preset dictionary is used (so every MSZIP block stands on its own).
[[nodiscard]] proven_err_t rp_deflate(proven_allocator_t alloc, const uint8_t *in, size_t n, int level, uint8_t **out,
                                      size_t *out_len);

// The same with a preset dictionary: dict[0..dn) (its last 32 KiB) may be referenced but is not
// written - an MSZIP block that continues the previous block's history (RFC-0013 E3).
[[nodiscard]] proven_err_t rp_deflate_dict(proven_allocator_t alloc, const uint8_t *dict, size_t dn, const uint8_t *in, size_t n,
                                           int level, uint8_t **out, size_t *out_len);

// One independently decodable part of a stream, as MSIX block maps need (RFC-0009; Windows'
// packaging API writes this, tests/fixtures/msix): in[0..n) as non-final blocks with no reference
// before it, then an empty stored block (00 00 FF FF). Parts put one after the other, closed by
// RP_DEFLATE_END (an empty final fixed block), are one valid stream.
[[nodiscard]] proven_err_t rp_deflate_segment(proven_allocator_t alloc, const uint8_t *in, size_t n, int level, uint8_t **out,
                                              size_t *out_len);
#define RP_DEFLATE_END "\x03\x00"

// Decodes one raw deflate stream from in[0..in_len) and appends the output to buf[*pos..cap);
// buf[0..*pos) is history that back-references may reach (MSZIP keeps the previous blocks'
// data as a dictionary). *used = input bytes consumed. Refuses output beyond cap and any
// malformed stream (PROVEN_ERR_INVALID_FORMAT); never reads outside `in`.
[[nodiscard]] proven_err_t rp_inflate(const uint8_t *in, size_t in_len, uint8_t *buf, size_t cap, size_t *pos,
                                      size_t *used);

#endif // RUBRAPACK_DEFLATE_H
