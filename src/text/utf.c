// src/text/utf.c - strict UTF-8 <-> UTF-16 conversion (see include/rubrapack/text.h), on
// proven_c_lib's converters (proven/utf.h); this file adds the offset of the first bad sequence.

#include "rubrapack/mem.h"
#include "rubrapack/text.h"

#include "proven/utf.h"

#include <stdckdint.h>
#include <string.h>

static rp_text_result_t fail(proven_err_t err, size_t offset) {
    return (rp_text_result_t){ .err = err, .units = 0, .offset = offset };
}

static proven_u8str_view_t view8(const uint8_t *src, size_t len) { return (proven_u8str_view_t){ .ptr = src, .size = len }; }

// Where the first ill-formed sequence starts (len when there is none).
static size_t bad_utf8_at(const uint8_t *src, size_t len) {
    for (size_t i = 0; i < len;) {
        proven_utf8_char_t c = proven_utf8_decode_next(view8(src, len), i);
        if (c.err != PROVEN_OK) return i;
        i += c.len;
    }
    return len;
}

static size_t bad_utf16_at(const proven_u16 *src, size_t len) {
    uint8_t buf[64];
    for (size_t i = 0; i < len;) {
        proven_utf_step_t st = proven_utf16_to_utf8_partial(src + i, len - i, buf, sizeof buf);
        if (st.err != PROVEN_OK && st.err != PROVEN_ERR_OUT_OF_BOUNDS) return i + st.consumed;
        if (st.consumed == 0) break;
        i += st.consumed;
    }
    return len;
}

rp_text_result_t rp_utf8_validate(const uint8_t *src, size_t len) {
    if (src == NULL && len != 0) return fail(PROVEN_ERR_INVALID_ARG, 0);
    size_t count = 0;
    for (size_t i = 0; i < len; ++count) {
        proven_utf8_char_t c = proven_utf8_decode_next(view8(src, len), i);
        if (c.err != PROVEN_OK) return fail(PROVEN_ERR_INVALID_ENCODING, i);
        i += c.len;
    }
    return (rp_text_result_t){ .err = PROVEN_OK, .units = count };
}

rp_text_result_t rp_utf8_to_utf16(const uint8_t *src, size_t len, proven_u16 *dst, size_t cap) {
    if (src == NULL && len != 0) return fail(PROVEN_ERR_INVALID_ARG, 0);
    proven_err_t err;
    size_t units = 0;
    if (dst == NULL) {
        proven_result_size_t r = proven_utf8_to_utf16_size(view8(src, len));
        err = r.err;
        units = r.value;
    } else {
        err = proven_utf8_to_utf16(view8(src, len), dst, cap, &units);
    }
    if (err == PROVEN_ERR_INVALID_ENCODING || err == PROVEN_ERR_NEED_MORE) return fail(PROVEN_ERR_INVALID_ENCODING, bad_utf8_at(src, len));
    if (err != PROVEN_OK) return fail(err, 0);
    return (rp_text_result_t){ .err = PROVEN_OK, .units = units };
}

rp_text_result_t rp_utf16_to_utf8(const proven_u16 *src, size_t len, uint8_t *dst, size_t cap) {
    if (src == NULL && len != 0) return fail(PROVEN_ERR_INVALID_ARG, 0);
    proven_err_t err;
    size_t units = 0;
    if (dst == NULL) {
        proven_result_size_t r = proven_utf16_to_utf8_size(src, len);
        err = r.err;
        units = r.value;
    } else {
        err = proven_utf16_to_utf8(src, len, dst, cap, &units);
    }
    if (err == PROVEN_ERR_INVALID_ENCODING || err == PROVEN_ERR_NEED_MORE) return fail(PROVEN_ERR_INVALID_ENCODING, bad_utf16_at(src, len));
    if (err != PROVEN_OK) return fail(err, 0);
    return (rp_text_result_t){ .err = PROVEN_OK, .units = units };
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

proven_err_t rp_name_fold(proven_allocator_t alloc, const char *name, char **out) {
    *out = NULL;
    size_t n = strlen(name);
    rp_text_result_t r = rp_utf8_to_utf16((const uint8_t *)name, n, NULL, 0);
    if (r.err != PROVEN_OK) {                       // not UTF-8: the bytes, ASCII upper-cased
        char *s = rp_mem_alloc(alloc, n + 1, 1);
        if (s == NULL) return PROVEN_ERR_NOMEM;
        for (size_t i = 0; i <= n; ++i) s[i] = name[i] >= 'a' && name[i] <= 'z' ? (char)(name[i] - 32) : name[i];
        *out = s;
        return PROVEN_OK;
    }
    size_t units = r.units;
    proven_u16 *w = rp_mem_alloc(alloc, units ? units : 1, sizeof *w);
    if (w == NULL) return PROVEN_ERR_NOMEM;
    proven_err_t err = rp_utf8_to_utf16((const uint8_t *)name, n, w, units).err;
    for (size_t i = 0; err == PROVEN_OK && i < units; ++i) {
        if (w[i] < 0xD800 || w[i] > 0xDFFF) w[i] = rp_upcase16(w[i]);
    }
    rp_text_result_t b = err == PROVEN_OK ? rp_utf16_to_utf8(w, units, NULL, 0) : (rp_text_result_t){ .err = err };
    char *s = b.err == PROVEN_OK ? rp_mem_alloc(alloc, b.units + 1, 1) : NULL;
    if (s != NULL && rp_utf16_to_utf8(w, units, (uint8_t *)s, b.units).err == PROVEN_OK) {
        s[b.units] = '\0';
        *out = s;
    } else {
        rp_mem_free(alloc, s);
        err = b.err != PROVEN_OK ? b.err : PROVEN_ERR_NOMEM;
    }
    rp_mem_free(alloc, w);
    return *out ? PROVEN_OK : err;
}
