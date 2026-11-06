#ifndef RUBRAPACK_CRYPTO_H
#define RUBRAPACK_CRYPTO_H

// include/rubrapack/crypto.h - hashes, HMAC and PBKDF2 for signing and key files
// (RFC-0007 1). All of it is proven_c_lib's: the hashes, HMAC and PBKDF2 (proven/hash.h,
// hash_legacy.h, hmac.h); keys and signatures are proven/sign.h's, used directly. Code that touches secrets (keys, passwords) does not branch on or index memory by
// secret values.

#include <stddef.h>
#include <stdint.h>

#include "proven/hash.h"
#include "proven/hash_legacy.h"
#include "proven/hmac.h"
#include "proven/types.h"

// ---- hashes, one interface --------------------------------------------------------------------

typedef enum { RP_HASH_SHA256, RP_HASH_SHA384, RP_HASH_SHA512 } rp_hash_alg_t;

typedef struct {
    rp_hash_alg_t alg;
    union {
        proven_sha256_t s256;
        proven_sha512_t s512;   // SHA-384 too
    } u;
} rp_hash_t;

enum { RP_HASH_MAX = 64 };

// SHA-1 (FIPS 180-4), only for identifying files in a catalog, as Windows' catalogs do (never for
// a signature).
typedef proven_sha1_t rp_sha1_t;
void rp_sha1_init(rp_sha1_t *s);
void rp_sha1_update(rp_sha1_t *s, const void *data, size_t len);
void rp_sha1_final(rp_sha1_t *s, uint8_t out[20]);

size_t rp_hash_size(rp_hash_alg_t alg);         // 32, 48, 64
size_t rp_hash_block(rp_hash_alg_t alg);        // 64, 128, 128
void rp_hash_init(rp_hash_t *h, rp_hash_alg_t alg);
void rp_hash_update(rp_hash_t *h, const void *data, size_t len);
void rp_hash_final(rp_hash_t *h, uint8_t *out);     // rp_hash_size(alg) bytes
void rp_hash(rp_hash_alg_t alg, const void *data, size_t len, uint8_t *out);

// The proven HMAC hash of `alg`.
static inline proven_hmac_hash_t rp_hash_mac(rp_hash_alg_t alg) {
    return alg == RP_HASH_SHA256 ? PROVEN_HMAC_SHA256 : alg == RP_HASH_SHA384 ? PROVEN_HMAC_SHA384 : PROVEN_HMAC_SHA512;
}

// ---- HMAC (RFC 2104) and PBKDF2 (RFC 8018) ---------------------------------------------------

typedef struct {
    proven_hmac_t m;
} rp_hmac_t;

void rp_hmac_init(rp_hmac_t *m, rp_hash_alg_t alg, const uint8_t *key, size_t key_len);
void rp_hmac_update(rp_hmac_t *m, const void *data, size_t len);
void rp_hmac_final(rp_hmac_t *m, uint8_t *out);
void rp_hmac(rp_hash_alg_t alg, const uint8_t *key, size_t key_len, const void *data, size_t len, uint8_t *out);

// PBKDF2 with HMAC-`alg`. iterations >= 1.
void rp_pbkdf2(rp_hash_alg_t alg, const uint8_t *pass, size_t pass_len, const uint8_t *salt, size_t salt_len,
               uint32_t iterations, uint8_t *out, size_t out_len);

// ---- the elliptic curves of EC keys (signatures are proven's: proven/sign.h) -------------------

typedef enum { RP_EC_P256, RP_EC_P384 } rp_ec_curve_t;
static inline size_t rp_ec_size(rp_ec_curve_t curve) { return curve == RP_EC_P256 ? 32 : 48; }     // scalar and coordinate bytes

// Wipes memory that held a secret (not optimised away).
void rp_wipe(void *p, size_t n);

// Constant-time equality of two buffers.
bool rp_ct_equal(const uint8_t *a, const uint8_t *b, size_t n);

#endif // RUBRAPACK_CRYPTO_H
