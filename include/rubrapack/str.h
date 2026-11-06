#ifndef RUBRAPACK_STR_H
#define RUBRAPACK_STR_H

// include/rubrapack/str.h - the few ASCII string helpers proven does not provide, in one place.
//
// proven already has the case-SENSITIVE view operations (proven_u8str_view_eq, _starts_with,
// _ends_with, _find, _slice, _trim, _split); use those directly rather than hand-rolling them.
// What proven has no equivalent for, and what the tool kept reimplementing, is ASCII
// case-insensitive comparison and an ASCII-only predicate. RFC-0021 ("proven_c_lib first")
// keeps libc/own code only where proven has none: this is that edge, collected into one
// implementation instead of a dozen copies.
//
// "ASCII" throughout: only bytes A-Z fold to a-z; every other byte, including UTF-8 lead and
// continuation bytes, is compared as-is. No locale, no Unicode case folding.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "proven/fmt.h"
#include "proven/u8str.h"

// ASCII lowercase of one byte; bytes outside A-Z pass through unchanged.
static inline uint8_t rp_ascii_lower(uint8_t c) {
    return c >= 'A' && c <= 'Z' ? (uint8_t)(c + 32) : c;
}

// ASCII uppercase of one byte; bytes outside a-z pass through unchanged.
static inline uint8_t rp_ascii_upper(uint8_t c) {
    return c >= 'a' && c <= 'z' ? (uint8_t)(c - 32) : c;
}

// Case-insensitive equality of two byte views (the view-term core the wrappers build on).
[[nodiscard]] static inline bool rp_ascii_view_ieq(proven_u8str_view_t a, proven_u8str_view_t b) {
    if (a.size != b.size) return false;
    for (proven_size_t i = 0; i < a.size; ++i) {
        if (rp_ascii_lower(a.ptr[i]) != rp_ascii_lower(b.ptr[i])) return false;
    }
    return true;
}

// Case-insensitive equality of the first n bytes of a and b (both must hold at least n bytes).
[[nodiscard]] static inline bool rp_ascii_ieq_n(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if (rp_ascii_lower((uint8_t)a[i]) != rp_ascii_lower((uint8_t)b[i])) return false;
    }
    return true;
}

// Case-insensitive equality of two NUL-terminated strings.
[[nodiscard]] static inline bool rp_ascii_ieq(const char *a, const char *b) {
    for (; *a && *b; ++a, ++b) {
        if (rp_ascii_lower((uint8_t)*a) != rp_ascii_lower((uint8_t)*b)) return false;
    }
    return *a == *b;
}

// strcmp-style sign, comparing UPPERCASED ASCII bytes. The fold is to uppercase, not lowercase:
// this orders results (e.g. the PRI writer's tree), and the direction decides where the bytes
// between 'Z' and 'a' (among them '_', common in MSIX asset names) fall relative to letters, so it
// must match the ordering the resource tooling expects. For the equality helpers above the
// direction is irrelevant; here it is not.
[[nodiscard]] static inline int rp_ascii_icmp(const char *a, const char *b) {
    for (;; ++a, ++b) {
        int x = rp_ascii_upper((uint8_t)*a), y = rp_ascii_upper((uint8_t)*b);
        if (x != y || x == 0) return x - y;
    }
}

// Whether s (NUL-terminated, may be NULL) ends with suffix, case-insensitively.
[[nodiscard]] static inline bool rp_ascii_ends_with_ci(const char *s, const char *suffix) {
    proven_u8str_view_t sv = proven_u8str_view_from_cstr(s);
    proven_u8str_view_t suf = proven_u8str_view_from_cstr(suffix);
    if (sv.size < suf.size) return false;
    return rp_ascii_view_ieq(proven_u8str_view_slice(sv, sv.size - suf.size, suf.size), suf);
}

// Whether every byte of s (may be NULL) is ASCII (below 0x80).
[[nodiscard]] static inline bool rp_is_ascii(const char *s) {
    for (; s && *s; ++s) {
        if ((unsigned char)*s >= 0x80) return false;
    }
    return true;
}

// Whether s equals one of a NULL-terminated array of C strings (case-sensitive).
[[nodiscard]] static inline bool rp_str_in_list(const char *s, const char *const *list) {
    for (size_t i = 0; list[i]; ++i) {
        if (strcmp(s, list[i]) == 0) return true;
    }
    return false;
}


// ---- text as proven views (DECISIONS 2026-11-02 "Hashes, TLS and text on proven") -------------
//
// The model's text (the IR) is proven_u8str_view_t over storage the model owns and ends with a NUL,
// so a view still is a C string where the C library or a message format needs one: rp_cs() costs
// nothing. Absent text is { NULL, 0 }; rp_has() tells it from "" (which proven compares equal).

static inline proven_u8str_view_t rp_v_view(proven_u8str_view_t v) { return v; }
static inline proven_u8str_view_t rp_v_cstr(const char *s) { return proven_u8str_view_from_cstr(s); }
// A view of either a view or a C string (NULL gives the absent view).
#define RP_V(x) _Generic((x), proven_u8str_view_t: rp_v_view, char *: rp_v_cstr, const char *: rp_v_cstr)(x)
// The C string of a view that ends in a NUL (the model's text, a literal's view); NULL when absent.
#define RP_NONE ((proven_u8str_view_t){ 0 })     // absent text
static inline const char *rp_cs(proven_u8str_view_t v) { return (const char *)v.ptr; }
static inline bool rp_has(proven_u8str_view_t v) { return v.ptr != NULL; }
// The same text to change in place, for the code that owns it (the model fixing a GUID's case).
static inline char *rp_ws(proven_u8str_view_t v) { return (char *)v.ptr; }
// Equality and order of text given as views or C strings (proven_u8str_view_eq / _cmp: the bytes,
// unsigned, the shorter first - strcmp's order for text without NUL bytes).
#define RP_EQ(a, b) (proven_u8str_view_eq(RP_V(a), RP_V(b)) != 0)
#define RP_CMP(a, b) proven_u8str_view_cmp(RP_V(a), RP_V(b))

// What snprintf would have returned for a proven_fmt_to call: the length of the whole text (also
// when it was cut to fit), or -1 for a format error. Text is formatted with proven_fmt_to.
static inline int rp_fmt_n(proven_fmt_result_t r) {
    return r.err == PROVEN_OK || r.err == PROVEN_ERR_OUT_OF_BOUNDS ? (int)r.required : -1;
}

// proven_fmt_to for the helpers that take a format naming at most two C strings, a then b (`{}`
// each): only as many arguments as the format names are passed, as proven requires. A format
// naming more than two is an error (PROVEN_ERR_INVALID_FORMAT, empty text).
static inline proven_fmt_result_t rp_fmt_strs(char *out, size_t cap, const char *fmt, const char *a, const char *b) {
    proven_result_size_t n = proven_fmt_arg_count(fmt);
    if (n.err != PROVEN_OK || n.value > 2) {
        if (cap) out[0] = '\0';
        return (proven_fmt_result_t){ .err = PROVEN_ERR_INVALID_FORMAT };
    }
    if (n.value == 0) return proven_fmt_to(out, cap, fmt);
    if (n.value == 1) return proven_fmt_to(out, cap, fmt, proven_arg_cstr(a));
    return proven_fmt_to(out, cap, fmt, proven_arg_cstr(a), proven_arg_cstr(b));
}

#endif // RUBRAPACK_STR_H
