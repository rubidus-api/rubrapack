#ifndef RUBRAPACK_NUM_H
#define RUBRAPACK_NUM_H

// include/rubrapack/num.h - reading an integer from text through proven's scanner
// (proven/scan.h): leading white space skipped, as strtol does, but a value that does not fit is
// refused instead of clamped. *end (when not NULL) is where reading stopped - s itself when it
// failed. false when there is no number or it overflows; *out is then left alone.

#include <stdint.h>
#include <string.h>

#include "proven/scan.h"

static inline proven_scan_t rp_num_scan(const char *s) {
    return proven_scan_init((proven_u8str_view_t){ .ptr = (const proven_byte_t *)s, .size = strlen(s) });
}

[[nodiscard]] static inline bool rp_read_u64(const char *s, const char **end, uint64_t *out) {
    proven_scan_t sc = rp_num_scan(s);
    proven_result_u64_t r = proven_scan_u64(&sc);
    if (end) *end = r.err == PROVEN_OK ? s + sc.cursor : s;
    if (r.err != PROVEN_OK) return false;
    *out = r.val;
    return true;
}

[[nodiscard]] static inline bool rp_read_i64(const char *s, const char **end, int64_t *out) {
    proven_scan_t sc = rp_num_scan(s);
    proven_result_i64_t r = proven_scan_i64(&sc);
    if (end) *end = r.err == PROVEN_OK ? s + sc.cursor : s;
    if (r.err != PROVEN_OK) return false;
    *out = r.val;
    return true;
}

// Hexadecimal digits, with or without "0x" in front.
[[nodiscard]] static inline bool rp_read_hex64(const char *s, const char **end, uint64_t *out) {
    proven_scan_t sc = rp_num_scan(s);
    proven_result_u64_t r = proven_scan_u64_hex(&sc);
    if (end) *end = r.err == PROVEN_OK ? s + sc.cursor : s;
    if (r.err != PROVEN_OK) return false;
    *out = r.val;
    return true;
}

// The decimal number at s, or 0 when there is none (atoi without its overflow).
static inline int64_t rp_int_or_zero(const char *s) {
    int64_t v = 0;
    return s && rp_read_i64(s, NULL, &v) ? v : 0;
}

#endif // RUBRAPACK_NUM_H
