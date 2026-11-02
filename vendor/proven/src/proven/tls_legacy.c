#include "proven_internal_tls.h"

/* The legacy set below the state machine (RFC-0011 decision T-8): HMAC with the two old
 * hashes, the PRF of TLS 1.0 and 1.1, and CBC records. None of it is reached unless a
 * configuration asks for a legacy suite or version by name.
 *
 * The one piece that needs care is opening a MAC-then-encrypt record. The receiver must
 * decrypt, find the padding, find the MAC in front of it and check both - and a receiver that
 * does less work for one kind of bad record than another tells an attacker which kind it was,
 * one byte of plaintext at a time (Vaudenay 2002; Lucky Thirteen, 2013). So everything after
 * decryption is done under masks: no branch and no memory index depends on a decrypted byte,
 * and the number of hash blocks compressed depends only on the record's length. */

/* ---- The three hashes, by their block functions ---- */

typedef struct {
    proven_u32 state[8];
    proven_u64 length;
    proven_byte_t block[64];
    proven_size_t fill;
    proven_tls_lh_t kind;
} proven_lh_t;

static proven_size_t lh_size(proven_tls_lh_t kind) { return kind == PROVEN_TLS_LH_MD5 ? 16 : kind == PROVEN_TLS_LH_SHA1 ? 20 : kind == PROVEN_TLS_LH_SHA256 ? 32 : 48; }

static void lh_start(proven_u32 state[8], proven_tls_lh_t kind) {
    static const proven_u32 legacy[5] = { 0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u, 0xc3d2e1f0u };
    static const proven_u32 sha256[8] = { 0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au, 0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u };
    for (int i = 0; i < 8; ++i) state[i] = kind == PROVEN_TLS_LH_SHA256 ? sha256[i] : i < 5 ? legacy[i] : 0;
}

static void lh_compress(proven_tls_lh_t kind, proven_u32 state[8], const proven_byte_t block[64]) {
    if (kind == PROVEN_TLS_LH_MD5) proven_md5_compress_(state, block);
    else if (kind == PROVEN_TLS_LH_SHA1) proven_sha1_compress_(state, block);
    else proven_sha256_compress_(state, block);
}

/* The digest from the state: MD5 writes its words little-endian, the other two big-endian. */
static void lh_digest(proven_tls_lh_t kind, const proven_u32 state[8], proven_byte_t *out) {
    const proven_size_t n = lh_size(kind);
    for (proven_size_t i = 0; i < n; ++i) {
        const unsigned shift = kind == PROVEN_TLS_LH_MD5 ? 8u * (unsigned)(i & 3) : 24u - 8u * (unsigned)(i & 3);
        out[i] = (proven_byte_t)(state[i / 4] >> shift);
    }
}

/* The eight bytes that end a padded message: its length in bits. */
static void lh_length(proven_tls_lh_t kind, proven_u64 bytes, proven_byte_t out[8]) {
    const proven_u64 bits = bytes * 8u;
    for (int i = 0; i < 8; ++i) out[i] = (proven_byte_t)(bits >> (kind == PROVEN_TLS_LH_MD5 ? 8 * i : 8 * (7 - i)));
}

static void lh_init(proven_lh_t *h, proven_tls_lh_t kind) {
    h->kind = kind; h->length = 0; h->fill = 0;
    lh_start(h->state, kind);
}

static void lh_update(proven_lh_t *h, const proven_byte_t *p, proven_size_t n) {
    h->length += n;
    for (proven_size_t i = 0; i < n; ++i) {
        h->block[h->fill++] = p[i];
        if (h->fill == 64) { lh_compress(h->kind, h->state, h->block); h->fill = 0; }
    }
}

static void lh_final(proven_lh_t *h, proven_byte_t *out) {
    proven_byte_t len[8];
    lh_length(h->kind, h->length, len);
    h->block[h->fill++] = 0x80;
    if (h->fill > 56) { while (h->fill < 64) h->block[h->fill++] = 0; lh_compress(h->kind, h->state, h->block); h->fill = 0; }
    while (h->fill < 56) h->block[h->fill++] = 0;
    for (int i = 0; i < 8; ++i) h->block[56 + i] = len[i];
    lh_compress(h->kind, h->state, h->block);
    lh_digest(h->kind, h->state, out);
    proven_mem_wipe((proven_mem_mut_t){ .ptr = (proven_byte_t *)h, .size = sizeof *h });
}

/* ---- HMAC (RFC 2104) ---- */

typedef struct { proven_lh_t inner, outer; } proven_lhmac_t;

static void lhmac_init(proven_lhmac_t *m, proven_tls_lh_t kind, proven_mem_view_t key) {
    proven_byte_t k[64] = { 0 }, pad[64];
    if (key.size > 64) {
        proven_lh_t h;
        lh_init(&h, kind);
        lh_update(&h, key.ptr, key.size);
        lh_final(&h, k);
    } else {
        for (proven_size_t i = 0; i < key.size; ++i) k[i] = key.ptr[i];
    }
    lh_init(&m->inner, kind);
    for (int i = 0; i < 64; ++i) pad[i] = k[i] ^ 0x36;
    lh_update(&m->inner, pad, 64);
    lh_init(&m->outer, kind);
    for (int i = 0; i < 64; ++i) pad[i] = k[i] ^ 0x5c;
    lh_update(&m->outer, pad, 64);
    proven_mem_wipe((proven_mem_mut_t){ .ptr = k, .size = sizeof k });
    proven_mem_wipe((proven_mem_mut_t){ .ptr = pad, .size = sizeof pad });
}

static void lhmac_final(proven_lhmac_t *m, proven_byte_t *out) {
    proven_byte_t inner[PROVEN_TLS_LH_MAX_SIZE];
    const proven_size_t n = lh_size(m->inner.kind);
    lh_final(&m->inner, inner);
    lh_update(&m->outer, inner, n);
    lh_final(&m->outer, out);
    proven_mem_wipe((proven_mem_mut_t){ .ptr = inner, .size = sizeof inner });
}

void proven_tls_legacy_hmac(proven_tls_lh_t hash, proven_mem_view_t key, proven_mem_view_t a, proven_mem_view_t b, proven_byte_t *out) {
    if (hash == PROVEN_TLS_LH_SHA384) {
        /* The one of the four that the public HMAC has: 128-byte blocks, another shape. */
        proven_hmac_t h;
        proven_byte_t full[PROVEN_HMAC_MAX_SIZE];
        (void)proven_hmac_init(&h, PROVEN_HMAC_SHA384, key);
        proven_hmac_update(&h, a);
        proven_hmac_update(&h, b);
        proven_hmac_final(&h, full);
        for (int i = 0; i < 48; ++i) out[i] = full[i];
        proven_mem_wipe((proven_mem_mut_t){ .ptr = full, .size = sizeof full });
        proven_mem_wipe((proven_mem_mut_t){ .ptr = (proven_byte_t *)&h, .size = sizeof h });
        return;
    }
    proven_lhmac_t m;
    lhmac_init(&m, hash, key);
    lh_update(&m.inner, a.ptr, a.size);
    lh_update(&m.inner, b.ptr, b.size);
    lhmac_final(&m, out);
}

/* ---- The PRF of TLS 1.0 and 1.1 ---- */

/* P_hash(secret, label | seed), XORed into `out`. */
static void p_hash_xor(proven_tls_lh_t kind, proven_mem_view_t secret, const char *label, proven_mem_view_t seed, proven_byte_t *out, proven_size_t out_len) {
    const proven_size_t n = lh_size(kind);
    proven_size_t label_len = 0;
    while (label[label_len]) ++label_len;
    proven_byte_t a[PROVEN_TLS_LH_MAX_SIZE], block[PROVEN_TLS_LH_MAX_SIZE];
    proven_lhmac_t m;
    lhmac_init(&m, kind, secret);                                   /* A(1) = HMAC(secret, label | seed) */
    lh_update(&m.inner, (const proven_byte_t *)label, label_len);
    lh_update(&m.inner, seed.ptr, seed.size);
    lhmac_final(&m, a);
    for (proven_size_t done = 0; done < out_len;) {
        lhmac_init(&m, kind, secret);                               /* HMAC(secret, A(i) | label | seed) */
        lh_update(&m.inner, a, n);
        lh_update(&m.inner, (const proven_byte_t *)label, label_len);
        lh_update(&m.inner, seed.ptr, seed.size);
        lhmac_final(&m, block);
        for (proven_size_t i = 0; i < n && done < out_len; ++i) out[done++] ^= block[i];
        proven_tls_legacy_hmac(kind, secret, (proven_mem_view_t){ .ptr = a, .size = n }, (proven_mem_view_t){ .ptr = NULL, .size = 0 }, a);
    }
    proven_mem_wipe((proven_mem_mut_t){ .ptr = a, .size = sizeof a });
    proven_mem_wipe((proven_mem_mut_t){ .ptr = block, .size = sizeof block });
}

void proven_tls10_prf(proven_mem_view_t secret, const char *label, proven_mem_view_t seed, proven_byte_t *out, proven_size_t out_len) {
    /* The two halves overlap by a byte when the secret's length is odd. */
    const proven_size_t half = (secret.size + 1) / 2;
    for (proven_size_t i = 0; i < out_len; ++i) out[i] = 0;
    p_hash_xor(PROVEN_TLS_LH_MD5, (proven_mem_view_t){ .ptr = secret.ptr, .size = half }, label, seed, out, out_len);
    p_hash_xor(PROVEN_TLS_LH_SHA1, (proven_mem_view_t){ .ptr = secret.ptr + (secret.size - half), .size = half }, label, seed, out, out_len);
}

/* ---- CBC records ---- */

/* All-ones when a < b, for values below 2^31. */
static proven_u32 ct_lt(proven_u32 a, proven_u32 b) { return (proven_u32)0 - ((a - b) >> 31); }
/* All-ones when a == b. */
static proven_u32 ct_eq(proven_u32 a, proven_u32 b) {
    const proven_u32 x = a ^ b;
    return (proven_u32)0 - (((x | ((proven_u32)0 - x)) >> 31) ^ 1u);
}

/* What the MAC covers before the content: seq | type | version | length. */
static void cbc_header(const proven_tls_keys_t *keys, const proven_tls_cbc_t *cbc, proven_byte_t type, proven_u32 len, proven_byte_t h[13]) {
    for (int i = 0; i < 8; ++i) h[i] = (proven_byte_t)(keys->seq >> (8 * (7 - i)));
    h[8] = type;
    h[9] = (proven_byte_t)(cbc->version >> 8); h[10] = (proven_byte_t)cbc->version;
    h[11] = (proven_byte_t)(len >> 8); h[12] = (proven_byte_t)len;
}

static void cbc_mac(const proven_tls_cbc_t *cbc, const proven_byte_t h[13], const proven_byte_t *data, proven_size_t len, proven_byte_t *out) {
    proven_tls_legacy_hmac((proven_tls_lh_t)cbc->mac, (proven_mem_view_t){ .ptr = cbc->mac_key, .size = cbc->mac_len },
                           (proven_mem_view_t){ .ptr = h, .size = 13 }, (proven_mem_view_t){ .ptr = data, .size = len }, out);
}

/* HMAC(key, h | data[0 .. len)) where `len` is SECRET and known only to lie in
 * [min_len, max_len], with `data` readable up to max_len + (something that is not read).
 *
 * The message is hashed block by block. Blocks that are message whatever `len` is are
 * compressed as they stand. From there to the last block the longest message could need,
 * every block is assembled under masks - a message byte, the 0x80 that ends the message, zero,
 * or the length - and compressed, and the state after the block that really is the last is
 * kept by a masked copy. The count of compressions depends on max_len alone. */
static void cbc_mac_ct(const proven_tls_cbc_t *cbc, const proven_byte_t h[13], const proven_byte_t *data,
                       proven_u32 len, proven_u32 min_len, proven_u32 max_len, proven_byte_t *out) {
    const proven_tls_lh_t kind = (proven_tls_lh_t)cbc->mac;
    const proven_size_t n = lh_size(kind);
    proven_u32 state[8], result[8];
    proven_byte_t block[64], bits[8];
    lh_start(state, kind);
    for (int i = 0; i < 64; ++i) block[i] = (proven_byte_t)((i < cbc->mac_len ? cbc->mac_key[i] : 0) ^ 0x36);
    lh_compress(kind, state, block);

    const proven_u32 mlen = 13 + len;                                 /* SECRET: header and content */
    const proven_u32 sure = (13 + min_len) / 64;                      /* blocks that are message for any len */
    const proven_u32 last = (13 + max_len + 8) / 64;                  /* the last block the longest message ends in */
    const proven_u32 final_block = (mlen + 8) >> 6;                   /* SECRET: where this message ends */
    lh_length(kind, (proven_u64)64 + mlen, bits);                     /* SECRET */
    for (int i = 0; i < 8; ++i) result[i] = 0;
    for (proven_u32 b = 0; b <= last; ++b) {
        for (proven_u32 j = 0; j < 64; ++j) {
            const proven_u32 o = b * 64 + j;
            const proven_byte_t v = o < 13 ? h[o] : o - 13 < max_len ? data[o - 13] : 0;
            if (b < sure) { block[j] = v; continue; }
            proven_u32 byte = (v & ct_lt(o, mlen)) | (0x80u & ct_eq(o, mlen));
            if (j >= 56) byte |= bits[j - 56] & ct_eq(b, final_block);
            block[j] = (proven_byte_t)byte;
        }
        lh_compress(kind, state, block);
        const proven_u32 keep = ct_eq(b, final_block);
        for (int i = 0; i < 8; ++i) result[i] |= state[i] & keep;
    }
    proven_byte_t inner[PROVEN_TLS_LH_MAX_SIZE];
    lh_digest(kind, result, inner);
    proven_lh_t outer;
    lh_init(&outer, kind);
    for (int i = 0; i < 64; ++i) block[i] = (proven_byte_t)((i < cbc->mac_len ? cbc->mac_key[i] : 0) ^ 0x5c);
    lh_update(&outer, block, 64);
    lh_update(&outer, inner, n);
    lh_final(&outer, out);
    proven_mem_wipe((proven_mem_mut_t){ .ptr = block, .size = sizeof block });
    proven_mem_wipe((proven_mem_mut_t){ .ptr = inner, .size = sizeof inner });
    proven_mem_wipe((proven_mem_mut_t){ .ptr = (proven_byte_t *)state, .size = sizeof state });
    proven_mem_wipe((proven_mem_mut_t){ .ptr = (proven_byte_t *)result, .size = sizeof result });
}

/* The same for HMAC-SHA-384, whose hash works in 128-byte blocks with a 16-byte length and
 * eight 64-bit words of state. Nothing else differs, and nothing is shared with the function
 * above on purpose: that one is the path every other suite uses. */
static void cbc_mac_ct_384(const proven_tls_cbc_t *cbc, const proven_byte_t h[13], const proven_byte_t *data,
                           proven_u32 len, proven_u32 min_len, proven_u32 max_len, proven_byte_t *out) {
    proven_sha384_t start;
    proven_u64 state[8], result[8];
    proven_byte_t block[128], bits[8];
    proven_sha384_init(&start);
    for (int i = 0; i < 8; ++i) { state[i] = start.state[i]; result[i] = 0; }
    for (int i = 0; i < 128; ++i) block[i] = (proven_byte_t)((i < cbc->mac_len ? cbc->mac_key[i] : 0) ^ 0x36);
    proven_sha512_compress_(state, block);

    const proven_u32 mlen = 13 + len;                                 /* SECRET */
    const proven_u32 sure = (13 + min_len) / 128;
    const proven_u32 last = (13 + max_len + 16) / 128;
    const proven_u32 final_block = (mlen + 16) >> 7;                  /* SECRET */
    const proven_u64 bitlen = ((proven_u64)128 + mlen) * 8u;          /* SECRET; the upper eight bytes of the length are zero */
    for (int i = 0; i < 8; ++i) bits[i] = (proven_byte_t)(bitlen >> (8 * (7 - i)));
    for (proven_u32 b = 0; b <= last; ++b) {
        for (proven_u32 j = 0; j < 128; ++j) {
            const proven_u32 o = b * 128 + j;
            const proven_byte_t v = o < 13 ? h[o] : o - 13 < max_len ? data[o - 13] : 0;
            if (b < sure) { block[j] = v; continue; }
            proven_u32 byte = (v & ct_lt(o, mlen)) | (0x80u & ct_eq(o, mlen));
            if (j >= 120) byte |= bits[j - 120] & ct_eq(b, final_block);
            block[j] = (proven_byte_t)byte;
        }
        proven_sha512_compress_(state, block);
        const proven_u64 keep = (proven_u64)0 - (proven_u64)(ct_eq(b, final_block) & 1u);
        for (int i = 0; i < 8; ++i) result[i] |= state[i] & keep;
    }
    proven_byte_t inner[48];
    for (int i = 0; i < 48; ++i) inner[i] = (proven_byte_t)(result[i / 8] >> (56 - 8 * (i % 8)));
    /* The outer hash is over a key block and 48 bytes: public lengths, the ordinary way. */
    proven_sha384_t outer;
    proven_byte_t full[PROVEN_SHA384_SIZE];
    proven_sha384_init(&outer);
    for (int i = 0; i < 128; ++i) block[i] = (proven_byte_t)((i < cbc->mac_len ? cbc->mac_key[i] : 0) ^ 0x5c);
    proven_sha384_update(&outer, (proven_mem_view_t){ .ptr = block, .size = 128 });
    proven_sha384_update(&outer, (proven_mem_view_t){ .ptr = inner, .size = 48 });
    proven_sha384_final(&outer, full);
    for (int i = 0; i < 48; ++i) out[i] = full[i];
    proven_mem_wipe((proven_mem_mut_t){ .ptr = block, .size = sizeof block });
    proven_mem_wipe((proven_mem_mut_t){ .ptr = inner, .size = sizeof inner });
    proven_mem_wipe((proven_mem_mut_t){ .ptr = (proven_byte_t *)state, .size = sizeof state });
    proven_mem_wipe((proven_mem_mut_t){ .ptr = (proven_byte_t *)result, .size = sizeof result });
    proven_mem_wipe((proven_mem_mut_t){ .ptr = (proven_byte_t *)&outer, .size = sizeof outer });
}

void proven_tls_cbc_set_keys(proven_tls_keys_t *client_write, proven_tls_keys_t *server_write, proven_tls_cbc_t cbc[2],
                             const proven_tls12_suite_t *suite, proven_u16 version, bool etm,
                             const proven_byte_t master[PROVEN_TLS12_MASTER_SIZE], const proven_byte_t client_random[32],
                             const proven_byte_t server_random[32]) {
    const proven_size_t mac_len = lh_size((proven_tls_lh_t)suite->mac), key_len = suite->key_len;
    const bool chained = version <= 0x0301;
    const proven_size_t need = 2 * mac_len + 2 * key_len + (chained ? 32 : 0);
    proven_byte_t seed[64], block[2 * PROVEN_TLS_LH_MAX_SIZE + 2 * 32 + 32];
    for (int i = 0; i < 32; ++i) { seed[i] = server_random[i]; seed[32 + i] = client_random[i]; }
    const proven_mem_view_t secret = { .ptr = master, .size = PROVEN_TLS12_MASTER_SIZE }, sv = { .ptr = seed, .size = sizeof seed };
    if (version >= 0x0303) proven_tls12_prf(suite->hash, secret, "key expansion", sv, block, need);
    else proven_tls10_prf(secret, "key expansion", sv, block, need);
    proven_tls_keys_t *keys[2] = { client_write, server_write };
    for (int side = 0; side < 2; ++side) {
        proven_tls_keys_t *k = keys[side];
        proven_tls_cbc_t *m = &cbc[side];
        proven_mem_wipe((proven_mem_mut_t){ .ptr = (proven_byte_t *)k, .size = sizeof *k });
        proven_mem_wipe((proven_mem_mut_t){ .ptr = (proven_byte_t *)m, .size = sizeof *m });
        k->active = proven_crypto_aes_gcm_init(&k->aes, block + 2 * mac_len + (proven_size_t)side * key_len, key_len);
        m->mac = suite->mac; m->mac_len = (proven_u8)mac_len; m->etm = etm; m->chained = chained; m->version = version;
        for (proven_size_t i = 0; i < mac_len; ++i) m->mac_key[i] = block[(proven_size_t)side * mac_len + i];
        if (chained) for (int i = 0; i < 16; ++i) m->iv[i] = block[2 * mac_len + 2 * key_len + (proven_size_t)side * 16 + (proven_size_t)i];
    }
    proven_mem_wipe((proven_mem_mut_t){ .ptr = block, .size = sizeof block });
}

proven_size_t proven_tls_cbc_seal(proven_tls_keys_t *keys, proven_tls_cbc_t *cbc, proven_byte_t type, proven_mem_view_t content,
                                  const proven_byte_t iv[16], proven_byte_t *out) {
    proven_byte_t h[13], chain[16];
    proven_byte_t *p = out + PROVEN_TLS_RECORD_HEADER;
    const proven_size_t iv_len = cbc->chained ? 0 : 16;
    for (int i = 0; i < 16; ++i) chain[i] = cbc->chained ? cbc->iv[i] : iv[i];
    for (proven_size_t i = 0; i < iv_len; ++i) p[i] = iv[i];
    proven_byte_t *text = p + iv_len;
    proven_size_t len = content.size;
    for (proven_size_t i = 0; i < len; ++i) text[i] = content.ptr[i];
    if (!cbc->etm) {
        cbc_header(keys, cbc, type, (proven_u32)content.size, h);
        cbc_mac(cbc, h, content.ptr, content.size, text + len);
        len += cbc->mac_len;
    }
    const proven_byte_t pad = (proven_byte_t)(15 - (len & 15));
    for (unsigned i = 0; i <= pad; ++i) text[len++] = pad;
    proven_crypto_aes_cbc_encrypt(&keys->aes, chain, text, len);
    if (cbc->chained) for (int i = 0; i < 16; ++i) cbc->iv[i] = chain[i];
    proven_size_t body = iv_len + len;
    if (cbc->etm) {
        cbc_header(keys, cbc, type, (proven_u32)body, h);
        cbc_mac(cbc, h, p, body, p + body);
        body += cbc->mac_len;
    }
    out[0] = type; out[1] = (proven_byte_t)(cbc->version >> 8); out[2] = (proven_byte_t)cbc->version;
    out[3] = (proven_byte_t)(body >> 8); out[4] = (proven_byte_t)body;
    keys->seq++;
    return PROVEN_TLS_RECORD_HEADER + body;
}

bool proven_tls_cbc_open(proven_tls_keys_t *keys, proven_tls_cbc_t *cbc, proven_byte_t *record, proven_size_t body_len,
                         proven_mem_mut_t *content) {
    const proven_size_t iv_len = cbc->chained ? 0 : 16, mac_len = cbc->mac_len;
    proven_byte_t h[13], mac[PROVEN_TLS_LH_MAX_SIZE], chain[16];
    proven_byte_t *p = record + PROVEN_TLS_RECORD_HEADER;
    if (body_len > PROVEN_TLS_MAX_CIPHERTEXT) return false;

    if (cbc->etm) {
        /* The MAC is over what travelled, and is checked before anything is decrypted. */
        if (body_len < iv_len + 16 + mac_len || ((body_len - iv_len - mac_len) & 15) != 0) return false;
        const proven_size_t covered = body_len - mac_len;
        cbc_header(keys, cbc, record[0], (proven_u32)covered, h);
        cbc_mac(cbc, h, p, covered, mac);
        if (!proven_mem_equal_ct((proven_mem_view_t){ .ptr = mac, .size = mac_len }, (proven_mem_view_t){ .ptr = p + covered, .size = mac_len })) return false;
        proven_byte_t *text = p + iv_len;
        const proven_size_t len = covered - iv_len;
        for (int i = 0; i < 16; ++i) chain[i] = cbc->chained ? cbc->iv[i] : p[i];
        proven_crypto_aes_cbc_decrypt(&keys->aes, chain, text, len);
        if (cbc->chained) for (int i = 0; i < 16; ++i) cbc->iv[i] = chain[i];
        /* The ciphertext was the peer's own, so its padding is not a secret from anyone; it is
         * still checked whole. */
        PROVEN_CT_PUBLIC(text, len);
        const proven_size_t pad = text[len - 1];
        if (pad + 1 > len) return false;
        for (proven_size_t i = 0; i <= pad; ++i) if (text[len - 1 - i] != pad) return false;
        if (len - pad - 1 > PROVEN_TLS_MAX_PLAINTEXT) return false;
        keys->seq++;
        content->ptr = text;
        content->size = len - pad - 1;
        return true;
    }

    /* MAC-then-encrypt. What is public: the record's length. */
    if (body_len < iv_len || ((body_len - iv_len) & 15) != 0) return false;
    const proven_u32 len = (proven_u32)(body_len - iv_len);
    if (len < ((mac_len + 1 + 15) & ~(proven_size_t)15)) return false;
    proven_byte_t *text = p + iv_len;
    for (int i = 0; i < 16; ++i) chain[i] = cbc->chained ? cbc->iv[i] : p[i];
    proven_crypto_aes_cbc_decrypt(&keys->aes, chain, text, len);
    if (cbc->chained) for (int i = 0; i < 16; ++i) cbc->iv[i] = chain[i];

    /* The padding: its length byte and that many bytes before it, all the same value, and room
     * left for a MAC. Up to 256 bytes are looked at whatever the length byte says. */
    const proven_u32 pad = text[len - 1];
    proven_u32 good = ~ct_lt(len, (proven_u32)mac_len + pad + 1);
    const proven_u32 look = len < 256 ? len : 256;
    proven_u32 diff = 0;
    for (proven_u32 i = 0; i < look; ++i) diff |= ~ct_lt(pad, i) & (text[len - 1 - i] ^ pad);
    good &= ct_eq(diff & 0xffu, 0);
    /* Padding that is wrong is treated as none, and the MAC is then computed and compared all
     * the same: it will not match, and that - not the padding - is what the answer is. */
    const proven_u32 used = pad & good;
    const proven_u32 max_len = len - (proven_u32)mac_len - 1;
    const proven_u32 min_len = max_len > 255 ? max_len - 255 : 0;
    const proven_u32 data_len = max_len - used;                       /* SECRET */
    cbc_header(keys, cbc, record[0], data_len, h);
    if (cbc->mac == PROVEN_TLS_LH_SHA384) cbc_mac_ct_384(cbc, h, text, data_len, min_len, max_len, mac);
    else cbc_mac_ct(cbc, h, text, data_len, min_len, max_len, mac);

    /* The record's MAC starts at data_len: gathered by a masked scan of every place it could be. */
    proven_byte_t theirs[PROVEN_TLS_LH_MAX_SIZE] = { 0 };
    for (proven_u32 i = min_len; i < len; ++i) {
        for (proven_u32 j = 0; j < mac_len; ++j) theirs[j] |= (proven_byte_t)(text[i] & ct_eq(i, data_len + j));
    }
    proven_u32 differ = 0;
    for (proven_u32 j = 0; j < mac_len; ++j) differ |= (proven_u32)(mac[j] ^ theirs[j]);
    good &= ct_eq(differ, 0);
    proven_mem_wipe((proven_mem_mut_t){ .ptr = mac, .size = sizeof mac });
    proven_mem_wipe((proven_mem_mut_t){ .ptr = theirs, .size = sizeof theirs });

    /* From here the verdict is public, and with it - when it is yes - the content. */
    volatile proven_u32 verdict = good;
    PROVEN_CT_PUBLIC(&verdict, sizeof verdict);
    if (verdict == 0) return false;
    proven_u32 shown = data_len;
    PROVEN_CT_PUBLIC(&shown, sizeof shown);
    PROVEN_CT_PUBLIC(text, shown);
    if (shown > PROVEN_TLS_MAX_PLAINTEXT) return false;
    keys->seq++;
    content->ptr = text;
    content->size = shown;
    return true;
}
