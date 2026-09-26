// src/core/buf.c - growable byte buffer (include/rubrapack/buf.h).

#include "rubrapack/buf.h"
#include "rubrapack/mem.h"

#include <stdckdint.h>
#include <stdio.h>
#include <string.h>

rp_buf_t rp_buf_new(proven_allocator_t alloc, size_t limit) {
    return (rp_buf_t){ .alloc = alloc, .limit = limit, .err = PROVEN_OK };
}

void rp_buf_free(rp_buf_t *b) {
    rp_mem_free(b->alloc, b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

static bool reserve(rp_buf_t *b, size_t more) {
    if (b->err != PROVEN_OK) return false;
    size_t need;
    if (ckd_add(&need, b->len, more)) {
        b->err = PROVEN_ERR_OVERFLOW;
        return false;
    }
    if (need > b->limit) {
        b->err = PROVEN_ERR_OUT_OF_BOUNDS;
        return false;
    }
    if (need <= b->cap) return true;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < need) cap = (cap > SIZE_MAX / 2) ? need : cap * 2;
    uint8_t *p = rp_mem_alloc(b->alloc, cap, 1);
    if (p == NULL) {
        b->err = PROVEN_ERR_NOMEM;
        return false;
    }
    if (b->len) memcpy(p, b->data, b->len);
    rp_mem_free(b->alloc, b->data);
    b->data = p;
    b->cap = cap;
    return true;
}

void rp_buf_put(rp_buf_t *b, const void *p, size_t n) {
    if (n == 0 || !reserve(b, n)) return;
    memcpy(b->data + b->len, p, n);
    b->len += n;
}

void rp_buf_puts(rp_buf_t *b, const char *s) { rp_buf_put(b, s, strlen(s)); }
void rp_buf_byte(rp_buf_t *b, uint8_t c) { rp_buf_put(b, &c, 1); }

void rp_buf_long(rp_buf_t *b, long long v) {
    char t[24];
    int n = snprintf(t, sizeof t, "%lld", v);
    if (n > 0) rp_buf_put(b, t, (size_t)n);
}

void rp_buf_u16le(rp_buf_t *b, uint16_t v) {
    uint8_t t[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
    rp_buf_put(b, t, 2);
}

void rp_buf_u32le(rp_buf_t *b, uint32_t v) {
    uint8_t t[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    rp_buf_put(b, t, 4);
}

void rp_buf_u64le(rp_buf_t *b, uint64_t v) {
    rp_buf_u32le(b, (uint32_t)v);
    rp_buf_u32le(b, (uint32_t)(v >> 32));
}

void rp_buf_zero(rp_buf_t *b, size_t n) {
    if (n == 0 || !reserve(b, n)) return;
    memset(b->data + b->len, 0, n);
    b->len += n;
}

proven_err_t rp_buf_take(rp_buf_t *b, uint8_t **out, size_t *len) {
    if (b->err != PROVEN_OK) {
        proven_err_t e = b->err;
        rp_buf_free(b);
        return e;
    }
    if (b->data == NULL && !reserve(b, 1)) return b->err;   // an empty result is still a block
    *out = b->data;
    *len = b->len;
    b->data = NULL;
    b->len = b->cap = 0;
    return PROVEN_OK;
}
