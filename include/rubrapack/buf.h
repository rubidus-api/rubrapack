#ifndef RUBRAPACK_BUF_H
#define RUBRAPACK_BUF_H

// include/rubrapack/buf.h - growable byte buffer with a sticky error: append freely, check
// `err` once at the end.

#include <stddef.h>
#include <stdint.h>

#include "proven/allocator.h"
#include "proven/types.h"

typedef struct {
    proven_allocator_t alloc;
    uint8_t           *data;
    size_t             len;
    size_t             cap;
    size_t             limit;   // refuse to grow past this many bytes
    proven_err_t       err;
} rp_buf_t;

[[nodiscard]] rp_buf_t rp_buf_new(proven_allocator_t alloc, size_t limit);
void rp_buf_free(rp_buf_t *b);

void rp_buf_put(rp_buf_t *b, const void *p, size_t n);
void rp_buf_puts(rp_buf_t *b, const char *s);
void rp_buf_byte(rp_buf_t *b, uint8_t c);
void rp_buf_long(rp_buf_t *b, long long v);         // decimal
void rp_buf_u16le(rp_buf_t *b, uint16_t v);
void rp_buf_u32le(rp_buf_t *b, uint32_t v);
void rp_buf_u64le(rp_buf_t *b, uint64_t v);
void rp_buf_zero(rp_buf_t *b, size_t n);

// Hands the bytes to the caller (free with rp_mem_free) and empties the buffer.
[[nodiscard]] proven_err_t rp_buf_take(rp_buf_t *b, uint8_t **out, size_t *len);

#endif // RUBRAPACK_BUF_H
