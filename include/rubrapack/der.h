#ifndef RUBRAPACK_DER_H
#define RUBRAPACK_DER_H

// include/rubrapack/der.h - DER reading and writing (X.690) for keys, certificates and CMS
// (RFC-0007 1). The reader keeps the rules of lowent_lang v1.3.0 lib/der.low (same author, MIT):
// every length is checked against the buffer in one place, only the shortest length form is
// accepted (that is what makes it DER), long-form lengths have at most 4 bytes, and it never
// recurses - callers walk nested structures with a cursor. Multi-byte tags are refused.
// BER (indefinite lengths, constructed strings), which PKCS#12 files from some tools use, is
// turned into DER by rp_ber_to_der first and is never read directly.

#include <stddef.h>
#include <stdint.h>

#include "proven/allocator.h"
#include "proven/types.h"
#include "rubrapack/buf.h"

enum {
    RP_DER_BOOLEAN = 0x01, RP_DER_INTEGER = 0x02, RP_DER_BIT_STRING = 0x03, RP_DER_OCTET_STRING = 0x04, RP_DER_NULL = 0x05,
    RP_DER_OID = 0x06, RP_DER_UTF8 = 0x0C, RP_DER_PRINTABLE = 0x13, RP_DER_IA5 = 0x16, RP_DER_UTCTIME = 0x17,
    RP_DER_GENTIME = 0x18, RP_DER_BMP = 0x1E, RP_DER_SEQUENCE = 0x30, RP_DER_SET = 0x31,
    RP_DER_CTX0 = 0xA0, RP_DER_CTX1 = 0xA1, RP_DER_CTX2 = 0xA2, RP_DER_CTX3 = 0xA3,
};

typedef struct {
    const uint8_t *p;
    size_t         n;
} rp_der_span_t;

typedef struct {
    uint8_t       tag;
    rp_der_span_t val;      // contents
    rp_der_span_t whole;    // tag, length and contents
} rp_der_t;

// Reads the next element of `s` and moves past it. False at the end of `s` or on anything that
// is not DER (the cursor is then left where it was).
[[nodiscard]] bool rp_der_read(rp_der_span_t *s, rp_der_t *e);
// The same, and the tag must be `tag`.
[[nodiscard]] bool rp_der_get(rp_der_span_t *s, uint8_t tag, rp_der_t *e);
// The tag of the next element, without moving; false at the end.
[[nodiscard]] bool rp_der_peek(const rp_der_span_t *s, uint8_t *tag);
// The contents of `e` as a cursor over its children.
static inline rp_der_span_t rp_der_inside(const rp_der_t *e) { return e->val; }
// An OBJECT IDENTIFIER element equals the given contents bytes.
bool rp_der_oid_is(const rp_der_t *e, const uint8_t *oid, size_t len);
// A non-negative INTEGER: its magnitude without the leading zero byte. False for a negative one.
[[nodiscard]] bool rp_der_uint(const rp_der_t *e, rp_der_span_t *out);
// A non-negative INTEGER that fits in 64 bits.
[[nodiscard]] bool rp_der_small(const rp_der_t *e, uint64_t *v);

// BER to DER: definite lengths, shortest length forms, constructed OCTET STRINGs joined into one.
// Nesting deeper than `max_depth` or output beyond `limit` bytes is refused.
[[nodiscard]] proven_err_t rp_ber_to_der(proven_allocator_t alloc, const uint8_t *in, size_t len, uint32_t max_depth,
                                         size_t limit, uint8_t **out, size_t *out_len);

// ---- writing ---------------------------------------------------------------------------------

void rp_der_put(rp_buf_t *b, uint8_t tag, const void *v, size_t n);        // one element
void rp_der_put_uint(rp_buf_t *b, const uint8_t *be, size_t n);          // INTEGER from unsigned bytes
void rp_der_put_small(rp_buf_t *b, uint64_t v);                          // INTEGER
void rp_der_put_oid(rp_buf_t *b, const uint8_t *oid, size_t n);          // OBJECT IDENTIFIER contents
void rp_der_put_null(rp_buf_t *b);
// Appends `tag`, the length of `child` and child's bytes, then frees child (and carries its error).
void rp_der_wrap(rp_buf_t *b, uint8_t tag, rp_buf_t *child);
// A SET OF: the encoded elements sorted as X.690 11.6 asks, then wrapped in `tag` (0x31 or a
// context tag such as [0] IMPLICIT, 0xA0).
void rp_der_set_of(rp_buf_t *b, uint8_t tag, const rp_der_span_t *elems, size_t n);

#endif // RUBRAPACK_DER_H
