#ifndef RP_WSTR_H
#define RP_WSTR_H

// src/ca/rp_wstr.h - the NUL-terminated UTF-16 strings of the Windows parts (the helper DLL, the
// cleanup task, the launcher, the menu part) on proven_c_lib's u16 views (proven/u16str.h), in
// the shapes the parts use: the C library's wcs* family, made bounded.
//
// wchar_t is 16-bit on Windows, the type of proven_u16, so the parts' strings pass as they are.
// A copy or an append that does not fit is not cut short (a path cut short is another path): the
// target is then empty and the result false, so whatever is done with it fails. Case-insensitive
// forms fold ASCII letters only, as _wcsicmp, _wcsnicmp and towlower do in the "C" locale the
// parts run in; CompareStringOrdinal stays where all of Unicode's letters must fold.
// tests/test_wstr.c (T201) checks each against the C library's definition.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#include "proven/u16str.h"

static inline proven_u16str_view_t rp_wv(const wchar_t *s) { return proven_u16str_view_from_cstr(s); }

static inline size_t rp_wlen(const wchar_t *s) { return proven_u16_cstr_len(s); }

// wcscmp(a, b) == 0, and wcscmp's order.
static inline bool rp_weq(const wchar_t *a, const wchar_t *b) { return proven_u16str_view_eq(rp_wv(a), rp_wv(b)); }
static inline int rp_wcmp(const wchar_t *a, const wchar_t *b) { return proven_u16str_view_cmp(rp_wv(a), rp_wv(b)); }

// wcsncmp(a, b, n) == 0: the first n units, or fewer up to an end, are the same.
static inline bool rp_weq_n(const wchar_t *a, const wchar_t *b, size_t n) {
    return proven_u16str_view_eq(proven_u16str_view_slice(rp_wv(a), 0, n), proven_u16str_view_slice(rp_wv(b), 0, n));
}

// s begins with p (wcsncmp(s, p, wcslen(p)) == 0).
static inline bool rp_wstarts(const wchar_t *s, const wchar_t *p) { return proven_u16str_view_starts_with(rp_wv(s), rp_wv(p)); }

// _wcsicmp(a, b) == 0 and _wcsnicmp(s, p, wcslen(p)) == 0: ASCII letters without case.
static inline bool rp_wieq(const wchar_t *a, const wchar_t *b) { return proven_u16str_view_eq_ascii_ci(rp_wv(a), rp_wv(b)); }
static inline bool rp_wistarts(const wchar_t *s, const wchar_t *p) {
    return proven_u16str_view_starts_with_ascii_ci(rp_wv(s), rp_wv(p));
}

static inline wchar_t rp_wlower(wchar_t c) { return proven_u16_ascii_lower(c); }

// wcschr, wcsrchr and wcsstr: a pointer into s, or NULL. A 0 unit is never looked for.
static inline wchar_t *rp_wchr(const wchar_t *s, wchar_t c) {
    size_t i = proven_u16str_view_find_unit(rp_wv(s), 0, c);
    return i == PROVEN_INDEX_NOT_FOUND ? NULL : (wchar_t *)s + i;
}
static inline wchar_t *rp_wrchr(const wchar_t *s, wchar_t c) {
    size_t i = proven_u16str_view_find_last_unit(rp_wv(s), c);
    return i == PROVEN_INDEX_NOT_FOUND ? NULL : (wchar_t *)s + i;
}
static inline wchar_t *rp_wstr(const wchar_t *s, const wchar_t *needle) {
    size_t i = proven_u16str_view_find(rp_wv(s), 0, rp_wv(needle));
    return i == PROVEN_INDEX_NOT_FOUND ? NULL : (wchar_t *)s + i;
}

// wcsspn(s, set) == wcslen(s): every unit of s is in set.
static inline bool rp_wall_in(const wchar_t *s, const wchar_t *set) {
    for (; *s; ++s)
        if (rp_wchr(set, *s) == NULL) return false;
    return true;
}

// Copy and append into dst of cap units. Too long: dst is empty, false.
static inline bool rp_wcpy(wchar_t *dst, size_t cap, const wchar_t *src) {
    proven_result_size_t r = proven_u16str_view_copy_to(dst, cap, rp_wv(src));
    if (r.err == PROVEN_OK) return true;
    if (cap) dst[0] = 0;
    return false;
}
static inline bool rp_wcat(wchar_t *dst, size_t cap, const wchar_t *src) {
    size_t n = rp_wlen(dst);
    if (n >= cap) {
        if (cap) dst[0] = 0;
        return false;
    }
    if (rp_wcpy(dst + n, cap - n, src)) return true;
    dst[0] = 0;
    return false;
}
// wcsncpy(dst, src, cap - 1) into a zeroed dst: what fits, cut, always ended.
static inline void rp_wcpy_cut(wchar_t *dst, size_t cap, const wchar_t *src) {
    (void)proven_u16str_view_copy_to(dst, cap, rp_wv(src));
}
// The same into an array, its size known.
#define RP_WCPY(arr, src) rp_wcpy((arr), sizeof(arr) / sizeof((arr)[0]), (src))
#define RP_WCAT(arr, src) rp_wcat((arr), sizeof(arr) / sizeof((arr)[0]), (src))
#define RP_WCPY_CUT(arr, src) rp_wcpy_cut((arr), sizeof(arr) / sizeof((arr)[0]), (src))

// wcstok over a buffer of the part's own: every unit of `delims` in s is made a 0 at once, so each
// token is ended in place, and the tokens are read with proven's tokenizer. Where wcstok ends a
// token only as it reaches it, this ends them all at the start: a loop that stops early leaves
// the rest split too, and s is read as tokens only, never again as a whole. s may be NULL.
typedef struct {
    proven_u16str_view_tokens_t it;
} rp_wtok_t;
static inline rp_wtok_t rp_wtok(wchar_t *s, const wchar_t *delims) {
    static const proven_u16 end[1] = { 0 };
    proven_u16str_view_t v = rp_wv(s);
    for (size_t i = 0; i < v.size; ++i)
        if (rp_wchr(delims, s[i])) s[i] = 0;
    return (rp_wtok_t){ proven_u16str_view_tokens(v, (proven_u16str_view_t){ end, 1 }) };
}
static inline wchar_t *rp_wtok_next(rp_wtok_t *t) {
    proven_u16str_view_t tok;
    return proven_u16str_view_tokens_next(&t->it, &tok) ? (wchar_t *)tok.ptr : NULL;
}

// An unsigned or signed integer at the front of s in base 10 or 16: its digits, after any ASCII
// white space (wcstoul's in the "C" locale); *end (when not NULL) is the first unit after them. No digit: 0 and *end = s, as
// wcstoul. Unlike wcstoul, a value too large for 64 bits, or a sign on the unsigned form, reads
// as no number (0).
static inline uint64_t rp_wtou(const wchar_t *s, unsigned base, const wchar_t **end) {
    const wchar_t *p = s;
    while (*p == L' ' || (*p >= L'\t' && *p <= L'\r')) ++p;
    size_t used = 0;
    proven_u64 v = 0;
    if (proven_u16str_view_parse_u64(rp_wv(p), base, &used, &v) != PROVEN_OK) {
        if (end) *end = s;
        return 0;
    }
    if (end) *end = p + used;
    return v;
}
static inline int64_t rp_wtoi(const wchar_t *s, unsigned base, const wchar_t **end) {
    const wchar_t *p = s;
    while (*p == L' ' || (*p >= L'\t' && *p <= L'\r')) ++p;
    size_t used = 0;
    proven_i64 v = 0;
    if (proven_u16str_view_parse_i64(rp_wv(p), base, &used, &v) != PROVEN_OK) {
        if (end) *end = s;
        return 0;
    }
    if (end) *end = p + used;
    return v;
}

#endif
