#ifndef RUBRAPACK_DEFLATE_H
#define RUBRAPACK_DEFLATE_H

// include/rubrapack/deflate.h - raw deflate (RFC 1951) encoder and decoder. Used for MSZIP
// cabinet blocks (MS-MCI) now, ZIP/MSIX and PNG later (RFC-0001 F5).

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

// Decodes one raw deflate stream from in[0..in_len) and appends the output to buf[*pos..cap);
// buf[0..*pos) is history that back-references may reach (MSZIP keeps the previous blocks'
// data as a dictionary). *used = input bytes consumed. Refuses output beyond cap and any
// malformed stream (PROVEN_ERR_INVALID_FORMAT); never reads outside `in`.
[[nodiscard]] proven_err_t rp_inflate(const uint8_t *in, size_t in_len, uint8_t *buf, size_t cap, size_t *pos,
                                      size_t *used);

#endif // RUBRAPACK_DEFLATE_H
