// src/model/ident.c - deterministic GUIDs and MSI keys (include/rubrapack/ident.h).

#include "rubrapack/ident.h"

#include <stdio.h>
#include <string.h>

#include "proven/hash.h"
#include "proven/random.h"

static void feed(proven_sha256_t *h, const void *p, size_t n) {
    uint8_t len[4] = { (uint8_t)n, (uint8_t)(n >> 8), (uint8_t)(n >> 16), (uint8_t)(n >> 24) };
    proven_sha256_update(h, (proven_mem_view_t){ len, 4 });
    if (n) proven_sha256_update(h, (proven_mem_view_t){ p, n });
}

static void uuid_text(const uint8_t b[16], char out[39]) {
    snprintf(out, 39, "{%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X}", b[0], b[1], b[2], b[3],
             b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
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
    static const char hex[] = "0123456789abcdef";
    out[0] = prefix;
    out[1] = '_';
    for (int k = 0; k < 10; ++k) {
        out[2 + 2 * k] = hex[d[k] >> 4];
        out[3 + 2 * k] = hex[d[k] & 15];
    }
    out[22] = '\0';
}
