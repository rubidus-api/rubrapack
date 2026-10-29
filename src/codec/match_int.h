// src/codec/match_int.h - what the deflate and LZX encoders' match finders share (deflate.c,
// lzx.c). Not a public header.

#ifndef RUBRAPACK_MATCH_INT_H
#define RUBRAPACK_MATCH_INT_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// How many of the first `lim` bytes of a and b are the same (compared eight at a time).
static inline size_t rp_match_len(const uint8_t *a, const uint8_t *b, size_t lim) {
    size_t l = 0;
    while (l + 8 <= lim) {
        uint64_t x, y;
        memcpy(&x, a + l, 8);
        memcpy(&y, b + l, 8);
        if (x != y) break;                      // the bytes below find which one
        l += 8;
    }
    while (l < lim && a[l] == b[l]) ++l;
    return l;
}

#endif // RUBRAPACK_MATCH_INT_H
