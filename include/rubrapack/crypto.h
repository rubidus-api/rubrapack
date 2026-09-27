#ifndef RUBRAPACK_CRYPTO_H
#define RUBRAPACK_CRYPTO_H

// include/rubrapack/crypto.h - hashes, HMAC, PBKDF2 and AES for signing and key files
// (RFC-0007 1). SHA-256 is proven's; SHA-384/512 are ported from lowent_lang v1.3.0
// (impl/src/low_sha512.h, same author, MIT). Code that touches secrets (keys, passwords) does
// not branch on or index memory by secret values.

#include <stddef.h>
#include <stdint.h>

#include "proven/hash.h"
#include "proven/types.h"

// ---- hashes, one interface --------------------------------------------------------------------

typedef enum { RP_HASH_SHA256, RP_HASH_SHA384, RP_HASH_SHA512 } rp_hash_alg_t;

typedef struct {
    rp_hash_alg_t alg;
    union {
        proven_sha256_t s256;
        struct {
            uint64_t h[8];
            uint8_t  b[128];
            uint64_t n;         // bytes hashed
        } s512;
    } u;
} rp_hash_t;

enum { RP_HASH_MAX = 64 };

size_t rp_hash_size(rp_hash_alg_t alg);         // 32, 48, 64
size_t rp_hash_block(rp_hash_alg_t alg);        // 64, 128, 128
void rp_hash_init(rp_hash_t *h, rp_hash_alg_t alg);
void rp_hash_update(rp_hash_t *h, const void *data, size_t len);
void rp_hash_final(rp_hash_t *h, uint8_t *out);     // rp_hash_size(alg) bytes
void rp_hash(rp_hash_alg_t alg, const void *data, size_t len, uint8_t *out);

// ---- HMAC (RFC 2104) and PBKDF2 (RFC 8018) ---------------------------------------------------

typedef struct {
    rp_hash_t inner, outer;
} rp_hmac_t;

void rp_hmac_init(rp_hmac_t *m, rp_hash_alg_t alg, const uint8_t *key, size_t key_len);
void rp_hmac_update(rp_hmac_t *m, const void *data, size_t len);
void rp_hmac_final(rp_hmac_t *m, uint8_t *out);
void rp_hmac(rp_hash_alg_t alg, const uint8_t *key, size_t key_len, const void *data, size_t len, uint8_t *out);

// PBKDF2 with HMAC-`alg`. iterations >= 1.
void rp_pbkdf2(rp_hash_alg_t alg, const uint8_t *pass, size_t pass_len, const uint8_t *salt, size_t salt_len,
               uint32_t iterations, uint8_t *out, size_t out_len);

// ---- AES (FIPS 197), CBC (SP 800-38A) --------------------------------------------------------

typedef struct {
    uint8_t rk[240];    // round keys
    int     rounds;     // 10, 12, 14
} rp_aes_t;

// key_len 16, 24 or 32; false otherwise.
bool rp_aes_init(rp_aes_t *a, const uint8_t *key, size_t key_len);
void rp_aes_encrypt(const rp_aes_t *a, const uint8_t in[16], uint8_t out[16]);
void rp_aes_decrypt(const rp_aes_t *a, const uint8_t in[16], uint8_t out[16]);
// CBC over whole blocks (len % 16 == 0; false otherwise); in and out may be the same buffer.
bool rp_aes_cbc_encrypt(const rp_aes_t *a, const uint8_t iv[16], const uint8_t *in, uint8_t *out, size_t len);
bool rp_aes_cbc_decrypt(const rp_aes_t *a, const uint8_t iv[16], const uint8_t *in, uint8_t *out, size_t len);

// Wipes memory that held a secret (not optimised away).
void rp_wipe(void *p, size_t n);

// Constant-time equality of two buffers.
bool rp_ct_equal(const uint8_t *a, const uint8_t *b, size_t n);

#endif // RUBRAPACK_CRYPTO_H
