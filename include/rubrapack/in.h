#ifndef RUBRAPACK_IN_H
#define RUBRAPACK_IN_H

// include/rubrapack/in.h - a bounds-checked view of a file someone else made.
//
// Every reader of an untrusted file (compound file, MSI tables, cabinet, ZIP, PE, registry hive,
// signatures) takes its bytes through an rp_in_t: by offset, because these formats are
// offset-based. A read outside the view returns 0 (or NULL, or an empty view) and sets `bad`,
// which stays set; the reader tests it where it decides, beside its own checks. The checks a
// reader makes give the precise reason; the view is the floor under them, so a check left out
// becomes a refusal, never a read outside memory. The writers keep include/rubrapack/le.h.
//
// A sub-view (rp_in_sub) has its own bad: a read outside it marks it, not its parent, so a reader
// tests every view it took a decision from. A field that may be absent (a shorter optional
// header, an extra field) is probed with rp_in_has, not read: a bare read would mark a valid
// short file bad. Zero is the value of a failed read, so a field used as a divisor, a shift or a
// count is still checked by the reader before use.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    const uint8_t *p;     // never NULL (an empty view points at rp_in__none), so p + 0 is defined
    size_t         n;
    bool           bad;   // sticky: some read went outside [0, n)
} rp_in_t;

static const uint8_t rp_in__none[1];

static inline rp_in_t rp_in(const uint8_t *p, size_t n) {
    if (p == NULL) return (rp_in_t){ .p = rp_in__none, .n = 0, .bad = false };
    return (rp_in_t){ .p = p, .n = n, .bad = false };
}

// True when [off, off + len) lies inside the view; never overflows.
static inline bool rp_in_has(const rp_in_t *in, size_t off, size_t len) {
    return off <= in->n && len <= in->n - off;
}

static inline const uint8_t *rp_in__at(rp_in_t *in, size_t off, size_t len) {
    if (!rp_in_has(in, off, len)) {
        in->bad = true;
        return NULL;
    }
    return in->p + off;
}

static inline uint8_t rp_in8(rp_in_t *in, size_t off) {
    const uint8_t *q = rp_in__at(in, off, 1);
    return q != NULL ? q[0] : 0;
}
static inline uint16_t rp_in16(rp_in_t *in, size_t off) {
    const uint8_t *q = rp_in__at(in, off, 2);
    return q != NULL ? (uint16_t)(q[0] | (q[1] << 8)) : 0;
}
static inline uint32_t rp_in32(rp_in_t *in, size_t off) {
    const uint8_t *q = rp_in__at(in, off, 4);
    return q != NULL ? (uint32_t)q[0] | ((uint32_t)q[1] << 8) | ((uint32_t)q[2] << 16) | ((uint32_t)q[3] << 24) : 0;
}
static inline uint64_t rp_in64(rp_in_t *in, size_t off) {
    const uint8_t *q = rp_in__at(in, off, 8);
    if (q == NULL) return 0;
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | q[i];
    return v;
}
static inline uint32_t rp_in32be(rp_in_t *in, size_t off) {
    const uint8_t *q = rp_in__at(in, off, 4);
    return q != NULL ? ((uint32_t)q[0] << 24) | ((uint32_t)q[1] << 16) | ((uint32_t)q[2] << 8) | (uint32_t)q[3] : 0;
}

// The len bytes at off, or NULL (and bad) when they are not all inside. len 0 at off <= n is a
// valid, empty range: the pointer is not NULL but must not be read.
static inline const uint8_t *rp_in_bytes(rp_in_t *in, size_t off, size_t len) {
    return rp_in__at(in, off, len);
}

// The view of [off, off + len): an empty view with bad set (and the parent's bad set) when it
// does not fit.
static inline rp_in_t rp_in_sub(rp_in_t *in, size_t off, size_t len) {
    const uint8_t *q = rp_in_bytes(in, off, len);
    if (q == NULL) return (rp_in_t){ .p = rp_in__none, .n = 0, .bad = true };
    return (rp_in_t){ .p = q, .n = len, .bad = false };
}

// Copies len bytes at off into dst; false (and bad, dst untouched) when they are not all inside.
static inline bool rp_in_copy(rp_in_t *in, size_t off, void *dst, size_t len) {
    const uint8_t *q = rp_in_bytes(in, off, len);
    if (q == NULL) return false;
    if (len != 0) memcpy(dst, q, len);
    return true;
}

#endif // RUBRAPACK_IN_H
