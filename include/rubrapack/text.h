#ifndef RUBRAPACK_TEXT_H
#define RUBRAPACK_TEXT_H

// include/rubrapack/text.h
//
// Strict UTF-8 <-> UTF-16 conversion. This module is the only place in rubrapack that
// changes a text encoding: everything inside the tool is UTF-8, and a UTF-16 boundary
// (Win32, CFB names, REGF, PE resources) converts here exactly once.
//
// "Strict" means both directions refuse anything that is not a sequence of Unicode scalar
// values: overlong UTF-8, UTF-8 encoded surrogates (ED A0..ED BF), code points above
// U+10FFFF, truncated or stray continuation bytes, and unpaired UTF-16 surrogates. There is
// no replacement character and no lossy mode. U+0000 is a valid scalar and converts; callers
// that must refuse NUL (paths, identifiers) check for it themselves.

#include <stddef.h>
#include <stdint.h>

#include "proven/allocator.h"
#include "proven/types.h"
#include "proven/u16str.h"
#include "proven/u8str.h"

// Outcome of a conversion or validation.
//   err     PROVEN_OK, PROVEN_ERR_INVALID_ENCODING, PROVEN_ERR_OUT_OF_BOUNDS (dst too small),
//           PROVEN_ERR_INVALID_ARG (NULL input with a nonzero length), PROVEN_ERR_OVERFLOW
//   units   on success: code units written (or needed, when dst is NULL)
//   offset  on PROVEN_ERR_INVALID_ENCODING: index, in source code units, of the first
//           unit of the bad sequence
typedef struct {
    proven_err_t err;
    size_t       units;
    size_t       offset;
} rp_text_result_t;

// Validates UTF-8. `units` is the number of scalar values on success.
[[nodiscard]] rp_text_result_t rp_utf8_validate(const uint8_t *src, size_t len);

// Converts UTF-8 to UTF-16 code units. With dst == NULL it only counts. When dst is too
// small nothing is promised about its contents and err is PROVEN_ERR_OUT_OF_BOUNDS.
[[nodiscard]] rp_text_result_t rp_utf8_to_utf16(const uint8_t *src, size_t len,
                                                proven_u16 *dst, size_t cap);

// Converts UTF-16 code units to UTF-8 bytes. Same contract as rp_utf8_to_utf16.
[[nodiscard]] rp_text_result_t rp_utf16_to_utf8(const proven_u16 *src, size_t len,
                                                uint8_t *dst, size_t cap);

// Allocating forms. On success *out owns a new string (destroy it with proven's
// u16str/u8str destroy); on failure *out is left untouched.
[[nodiscard]] rp_text_result_t rp_utf8_to_u16str(proven_allocator_t alloc, proven_u8str_view_t src,
                                                 proven_u16str_t *out);
[[nodiscard]] rp_text_result_t rp_utf16_to_u8str(proven_allocator_t alloc, proven_u16str_view_t src,
                                                 proven_u8str_t *out);

// Unicode Normalization Form C (UAX #15; RFC-0006 L3), Unicode 17.0 data from the UCD
// (src/text/nfc_tables.c). The input must be valid UTF-8 (PROVEN_ERR_INVALID_ENCODING otherwise);
// *out is NUL-terminated, free it with rp_mem_free.
[[nodiscard]] proven_err_t rp_nfc(proven_allocator_t alloc, const uint8_t *src, size_t len, uint8_t **out, size_t *out_len);

// Whether valid UTF-8 text is already in NFC (false for invalid UTF-8 or when out of memory).
[[nodiscard]] bool rp_is_nfc(proven_allocator_t alloc, const uint8_t *src, size_t len);

#endif // RUBRAPACK_TEXT_H
