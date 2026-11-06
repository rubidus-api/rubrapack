// src/model/ident.c - deterministic GUIDs and MSI keys (include/rubrapack/ident.h).

#include "rubrapack/ident.h"
#include "rubrapack/str.h"

#include <stdio.h>
#include <string.h>

#include "proven/encode.h"
#include "proven/hash.h"
#include "proven/random.h"

static void feed(proven_sha256_t *h, const void *p, size_t n) {
    uint8_t len[4] = { (uint8_t)n, (uint8_t)(n >> 8), (uint8_t)(n >> 16), (uint8_t)(n >> 24) };
    proven_sha256_update(h, (proven_mem_view_t){ len, 4 });
    if (n) proven_sha256_update(h, (proven_mem_view_t){ p, n });
}

static void uuid_text(const uint8_t b[16], char out[39]) {
    (void)proven_fmt_to(out, 39, "{{{:02X}{:02X}{:02X}{:02X}-{:02X}{:02X}-{:02X}{:02X}-{:02X}{:02X}-{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}}}", proven_arg_u32(b[0]), proven_arg_u32(b[1]), proven_arg_u32(b[2]), proven_arg_u32(b[3]), proven_arg_u32(b[4]), proven_arg_u32(b[5]), proven_arg_u32(b[6]), proven_arg_u32(b[7]), proven_arg_u32(b[8]), proven_arg_u32(b[9]), proven_arg_u32(b[10]), proven_arg_u32(b[11]), proven_arg_u32(b[12]), proven_arg_u32(b[13]), proven_arg_u32(b[14]), proven_arg_u32(b[15]));
}

void rp_uuid_derive(const char *tag, const char *const *fields, size_t count, char out[39]) {
    proven_sha256_t h;
    proven_sha256_init(&h);
    feed(&h, tag, strlen(tag));
    uint8_t version[4] = { 1, 0, 0, 0 };
    proven_sha256_update(&h, (proven_mem_view_t){ version, 4 });
    for (size_t k = 0; k < count; ++k) feed(&h, fields[k], strlen(fields[k]));
    uint8_t d[PROVEN_SHA256_SIZE];
    proven_sha256_final(&h, d);
    d[6] = (uint8_t)((d[6] & 0x0F) | 0x80);     // version 8
    d[8] = (uint8_t)((d[8] & 0x3F) | 0x80);     // variant 10
    uuid_text(d, out);
}

proven_err_t rp_uuid_random(char out[39]) {
    uint8_t b[16];
    if (!proven_random_bytes(b, sizeof b)) return PROVEN_ERR_IO;
    b[6] = (uint8_t)((b[6] & 0x0F) | 0x40);     // version 4
    b[8] = (uint8_t)((b[8] & 0x3F) | 0x80);
    uuid_text(b, out);
    return PROVEN_OK;
}

void rp_key_derive(char prefix, const char *text, char out[23]) {
    uint8_t d[PROVEN_SHA256_SIZE];
    proven_sha256((proven_mem_view_t){ (const proven_byte_t *)text, strlen(text) }, d);
    out[0] = prefix;
    out[1] = '_';
    proven_size_t w = 0;
    if (proven_hex_encode((proven_mem_view_t){ d, 10 }, (proven_byte_t *)out + 2, 20, &w) != PROVEN_OK) w = 0;
    out[2 + w] = '\0';
}
