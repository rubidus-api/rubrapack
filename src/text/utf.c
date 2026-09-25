// src/text/utf.c - strict UTF-8 <-> UTF-16 conversion (see include/rubrapack/text.h).

#include "rubrapack/text.h"

#include <stdckdint.h>
#include <string.h>

// Decodes one scalar value starting at src[i]. Returns the sequence length (1-4), or 0 when
// the bytes at src[i] do not begin a well-formed sequence (Unicode Table 3-7).
static size_t decode_utf8(const uint8_t *src, size_t len, size_t i, uint32_t *out) {
    uint8_t b0 = src[i];
    if (b0 < 0x80) {
        *out = b0;
        return 1;
    }

    size_t need;
    uint32_t cp;
    uint8_t lo = 0x80;
    uint8_t hi = 0xBF;
    if (b0 >= 0xC2 && b0 <= 0xDF) {
        need = 2;
        cp = b0 & 0x1Fu;
    } else if (b0 >= 0xE0 && b0 <= 0xEF) {
        need = 3;
        cp = b0 & 0x0Fu;
        if (b0 == 0xE0) lo = 0xA0;      // overlong
        if (b0 == 0xED) hi = 0x9F;      // UTF-16 surrogates
    } else if (b0 >= 0xF0 && b0 <= 0xF4) {
        need = 4;
        cp = b0 & 0x07u;
        if (b0 == 0xF0) lo = 0x90;      // overlong
        if (b0 == 0xF4) hi = 0x8F;      // above U+10FFFF
    } else {
        return 0;                       // continuation byte, C0/C1, F5..FF
    }

    if (len - i < need) return 0;
    for (size_t k = 1; k < need; ++k) {
        uint8_t b = src[i + k];
        uint8_t min = (k == 1) ? lo : 0x80;
        uint8_t max = (k == 1) ? hi : 0xBF;
        if (b < min || b > max) return 0;
        cp = (cp << 6) | (b & 0x3Fu);
    }
    *out = cp;
    return need;
}

static rp_text_result_t fail(proven_err_t err, size_t offset) {
    return (rp_text_result_t){ .err = err, .units = 0, .offset = offset };
}

rp_text_result_t rp_utf8_validate(const uint8_t *src, size_t len) {
    if (src == NULL && len != 0) return fail(PROVEN_ERR_INVALID_ARG, 0);
    size_t count = 0;
    for (size_t i = 0; i < len;) {
        uint32_t cp;
        size_t n = decode_utf8(src, len, i, &cp);
        if (n == 0) return fail(PROVEN_ERR_INVALID_ENCODING, i);
        i += n;
        ++count;
    }
    return (rp_text_result_t){ .err = PROVEN_OK, .units = count };
}

rp_text_result_t rp_utf8_to_utf16(const uint8_t *src, size_t len, proven_u16 *dst, size_t cap) {
    if (src == NULL && len != 0) return fail(PROVEN_ERR_INVALID_ARG, 0);
    size_t out = 0;
    for (size_t i = 0; i < len;) {
        uint32_t cp;
        size_t n = decode_utf8(src, len, i, &cp);
        if (n == 0) return fail(PROVEN_ERR_INVALID_ENCODING, i);
        size_t units = (cp >= 0x10000) ? 2 : 1;
        if (dst != NULL) {
            if (cap - out < units) return fail(PROVEN_ERR_OUT_OF_BOUNDS, i);
            if (units == 1) {
                dst[out] = (proven_u16)cp;
            } else {
                uint32_t v = cp - 0x10000;
                dst[out] = (proven_u16)(0xD800 | (v >> 10));
                dst[out + 1] = (proven_u16)(0xDC00 | (v & 0x3FFu));
            }
        }
        if (ckd_add(&out, out, units)) return fail(PROVEN_ERR_OVERFLOW, i);
        i += n;
    }
    return (rp_text_result_t){ .err = PROVEN_OK, .units = out };
}

rp_text_result_t rp_utf16_to_utf8(const proven_u16 *src, size_t len, uint8_t *dst, size_t cap) {
    if (src == NULL && len != 0) return fail(PROVEN_ERR_INVALID_ARG, 0);
    size_t out = 0;
    for (size_t i = 0; i < len;) {
        uint32_t cp = src[i];
        size_t used = 1;
        if (cp >= 0xD800 && cp <= 0xDBFF) {
            if (i + 1 >= len) return fail(PROVEN_ERR_INVALID_ENCODING, i);
            uint32_t lo = src[i + 1];
            if (lo < 0xDC00 || lo > 0xDFFF) return fail(PROVEN_ERR_INVALID_ENCODING, i);
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            used = 2;
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            return fail(PROVEN_ERR_INVALID_ENCODING, i);
        }

        uint8_t buf[4];
        size_t n;
        if (cp < 0x80) {
            buf[0] = (uint8_t)cp;
            n = 1;
        } else if (cp < 0x800) {
            buf[0] = (uint8_t)(0xC0 | (cp >> 6));
            buf[1] = (uint8_t)(0x80 | (cp & 0x3F));
            n = 2;
        } else if (cp < 0x10000) {
            buf[0] = (uint8_t)(0xE0 | (cp >> 12));
            buf[1] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
            buf[2] = (uint8_t)(0x80 | (cp & 0x3F));
            n = 3;
        } else {
            buf[0] = (uint8_t)(0xF0 | (cp >> 18));
            buf[1] = (uint8_t)(0x80 | ((cp >> 12) & 0x3F));
            buf[2] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
            buf[3] = (uint8_t)(0x80 | (cp & 0x3F));
            n = 4;
        }
        if (dst != NULL) {
            if (cap - out < n) return fail(PROVEN_ERR_OUT_OF_BOUNDS, i);
            memcpy(dst + out, buf, n);
        }
        if (ckd_add(&out, out, n)) return fail(PROVEN_ERR_OVERFLOW, i);
        i += used;
    }
    return (rp_text_result_t){ .err = PROVEN_OK, .units = out };
}

// The allocating forms convert into a scratch block from `alloc`, then hand it to proven's
// string constructor, which adds the terminator and owns the result.
static rp_text_result_t scratch(proven_allocator_t alloc, size_t units, size_t unit_size,
                                proven_mem_mut_t *mem) {
    size_t bytes;
    if (ckd_mul(&bytes, units == 0 ? 1 : units, unit_size)) return fail(PROVEN_ERR_OVERFLOW, 0);
    proven_result_mem_mut_t r = alloc.alloc_fn(alloc.ctx, bytes, alignof(proven_u16));
    if (r.err != PROVEN_OK) return fail(r.err, 0);
    *mem = r.value;
    return (rp_text_result_t){ .err = PROVEN_OK };
}

rp_text_result_t rp_utf8_to_u16str(proven_allocator_t alloc, proven_u8str_view_t src,
                                   proven_u16str_t *out) {
    if (!proven_alloc_is_valid(alloc) || out == NULL) return fail(PROVEN_ERR_INVALID_ARG, 0);
    rp_text_result_t r = rp_utf8_to_utf16(src.ptr, src.size, NULL, 0);
    if (r.err != PROVEN_OK) return r;

    proven_mem_mut_t mem;
    rp_text_result_t s = scratch(alloc, r.units, sizeof(proven_u16), &mem);
    if (s.err != PROVEN_OK) return s;
    proven_u16 *units = (proven_u16 *)(void *)mem.ptr;
    r = rp_utf8_to_utf16(src.ptr, src.size, units, r.units);
    if (r.err == PROVEN_OK) {
        proven_result_u16str_t made = proven_u16str_create_from_view(
            alloc, (proven_u16str_view_t){ .ptr = units, .size = r.units });
        if (made.err == PROVEN_OK) {
            *out = made.value;
        } else {
            r = fail(made.err, 0);
        }
    }
    alloc.free_fn(alloc.ctx, mem.ptr);
    return r;
}

rp_text_result_t rp_utf16_to_u8str(proven_allocator_t alloc, proven_u16str_view_t src,
                                   proven_u8str_t *out) {
    if (!proven_alloc_is_valid(alloc) || out == NULL) return fail(PROVEN_ERR_INVALID_ARG, 0);
    rp_text_result_t r = rp_utf16_to_utf8(src.ptr, src.size, NULL, 0);
    if (r.err != PROVEN_OK) return r;

    proven_mem_mut_t mem;
    rp_text_result_t s = scratch(alloc, r.units, 1, &mem);
    if (s.err != PROVEN_OK) return s;
    r = rp_utf16_to_utf8(src.ptr, src.size, mem.ptr, r.units);
    if (r.err == PROVEN_OK) {
        proven_result_u8str_t made = proven_u8str_create_from_view(
            alloc, (proven_u8str_view_t){ .ptr = mem.ptr, .size = r.units });
        if (made.err == PROVEN_OK) {
            *out = made.value;
        } else {
            r = fail(made.err, 0);
        }
    }
    alloc.free_fn(alloc.ctx, mem.ptr);
    return r;
}
