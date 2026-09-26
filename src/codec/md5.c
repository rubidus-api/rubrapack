// src/codec/md5.c - MD5 from RFC 1321 (include/rubrapack/md5.h).

#include "rubrapack/md5.h"

#include <string.h>

static uint32_t rol(uint32_t x, unsigned s) { return (x << s) | (x >> (32 - s)); }

// T[i] = floor(2^32 * |sin(i + 1)|), RFC 1321 section 3.4.
static const uint32_t T[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};

static const unsigned S[4][4] = { { 7, 12, 17, 22 }, { 5, 9, 14, 20 }, { 4, 11, 16, 23 }, { 6, 10, 15, 21 } };

static void block(rp_md5_t *c, const uint8_t *p) {
    uint32_t x[16];
    for (int k = 0; k < 16; ++k) {
        x[k] = (uint32_t)p[4 * k] | ((uint32_t)p[4 * k + 1] << 8) | ((uint32_t)p[4 * k + 2] << 16) |
               ((uint32_t)p[4 * k + 3] << 24);
    }
    uint32_t a = c->state[0], b = c->state[1], cc = c->state[2], d = c->state[3];
    for (int i = 0; i < 64; ++i) {
        uint32_t f;
        int g, round = i / 16;
        if (round == 0) { f = (b & cc) | (~b & d); g = i; }
        else if (round == 1) { f = (d & b) | (~d & cc); g = (5 * i + 1) % 16; }
        else if (round == 2) { f = b ^ cc ^ d; g = (3 * i + 5) % 16; }
        else { f = cc ^ (b | ~d); g = (7 * i) % 16; }
        uint32_t t = d;
        d = cc;
        cc = b;
        b = b + rol(a + f + T[i] + x[g], S[round][i % 4]);
        a = t;
    }
    c->state[0] += a;
    c->state[1] += b;
    c->state[2] += cc;
    c->state[3] += d;
}

void rp_md5_init(rp_md5_t *c) {
    c->state[0] = 0x67452301;
    c->state[1] = 0xefcdab89;
    c->state[2] = 0x98badcfe;
    c->state[3] = 0x10325476;
    c->length = 0;
    c->used = 0;
}

void rp_md5_update(rp_md5_t *c, const void *data, size_t len) {
    const uint8_t *p = data;
    c->length += len;
    while (len > 0) {
        size_t take = 64 - c->used < len ? 64 - c->used : len;
        memcpy(c->block + c->used, p, take);
        c->used += take;
        p += take;
        len -= take;
        if (c->used == 64) {
            block(c, c->block);
            c->used = 0;
        }
    }
}

void rp_md5_final(rp_md5_t *c, uint8_t out[16]) {
    uint64_t bits = c->length * 8;
    uint8_t pad = 0x80;
    rp_md5_update(c, &pad, 1);
    uint8_t zero = 0;
    while (c->used != 56) rp_md5_update(c, &zero, 1);
    uint8_t len[8];
    for (int k = 0; k < 8; ++k) len[k] = (uint8_t)(bits >> (8 * k));
    rp_md5_update(c, len, 8);
    for (int k = 0; k < 4; ++k) {
        for (int j = 0; j < 4; ++j) out[4 * k + j] = (uint8_t)(c->state[k] >> (8 * j));
    }
}

void rp_md5(const void *data, size_t len, uint8_t out[16]) {
    rp_md5_t c;
    rp_md5_init(&c);
    rp_md5_update(&c, data, len);
    rp_md5_final(&c, out);
}
