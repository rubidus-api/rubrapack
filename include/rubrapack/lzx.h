#ifndef RUBRAPACK_LZX_H
#define RUBRAPACK_LZX_H

// include/rubrapack/lzx.h - LZX as cabinet folders use it (MS-PATCH 2 "LZX DELTA" is its superset;
// format notes F4 "LZX"). A folder is one stream cut into frames of 32768 output bytes (the last one
// shorter), each frame one CFDATA block ending on a 16-bit boundary; the window (2^15 to 2^21
// bytes), the repeated offsets and the trees carry on from frame to frame.

#include <stddef.h>
#include <stdint.h>

#include "proven/allocator.h"
#include "proven/types.h"

enum { RP_LZX_FRAME = 32768, RP_LZX_MIN_WBITS = 15, RP_LZX_MAX_WBITS = 21 };

typedef struct rp_lzx_enc rp_lzx_enc_t;
typedef struct rp_lzx_dec rp_lzx_dec_t;

// An encoder for one folder with a window of 2^wbits bytes.
[[nodiscard]] proven_err_t rp_lzx_enc_new(proven_allocator_t alloc, unsigned wbits, rp_lzx_enc_t **out);
// Compresses the folder's next frame, in[0..n) with 1 <= n <= 32768 (only the last frame may be
// shorter). *out stays valid until the next call. Deterministic: verbatim blocks, one per frame, no
// match crossing a frame, no E8 translation.
[[nodiscard]] proven_err_t rp_lzx_enc_frame(rp_lzx_enc_t *e, const uint8_t *in, size_t n, const uint8_t **out, size_t *out_len);
void rp_lzx_enc_free(rp_lzx_enc_t *e);

// A decoder for one folder: every block type, matches and blocks that cross frames, E8 translation.
[[nodiscard]] proven_err_t rp_lzx_dec_new(proven_allocator_t alloc, unsigned wbits, rp_lzx_dec_t **out);
// Decodes the folder's next frame from its CFDATA bytes in[0..in_len) into out[0..out_len).
// PROVEN_ERR_INVALID_FORMAT for anything malformed; never reads or writes out of bounds.
[[nodiscard]] proven_err_t rp_lzx_dec_frame(rp_lzx_dec_t *d, const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len);
void rp_lzx_dec_free(rp_lzx_dec_t *d);

#endif // RUBRAPACK_LZX_H
