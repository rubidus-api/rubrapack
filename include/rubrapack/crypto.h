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

// ---- ECDSA over P-256 and P-384 (FIPS 186-4, RFC 6979; src/crypto/ecdsa.c) ---------------------
// Ported and vector-tested, but not used for signing packages until Windows is seen to accept it
// in each format (RFC-0007 S1).

typedef enum { RP_EC_P256, RP_EC_P384 } rp_ec_curve_t;

size_t rp_ec_size(rp_ec_curve_t curve);         // 32 or 48: scalar and coordinate bytes
// The public key of private scalar d (big-endian, rp_ec_size bytes); false when d is 0 or >= n.
[[nodiscard]] bool rp_ec_public(rp_ec_curve_t curve, const uint8_t *d, uint8_t *qx, uint8_t *qy);
// Deterministic ECDSA (RFC 6979, HMAC with `alg`): r and s get rp_ec_size bytes. The signature
// is verified with the public key before it is returned (PROVEN_ERR_INVALID_STATE if not).
[[nodiscard]] proven_err_t rp_ecdsa_sign(rp_ec_curve_t curve, const uint8_t *d, rp_hash_alg_t alg, const uint8_t *digest, uint8_t *r,
                                         uint8_t *s);
[[nodiscard]] bool rp_ecdsa_verify(rp_ec_curve_t curve, const uint8_t *qx, const uint8_t *qy, rp_hash_alg_t alg, const uint8_t *digest,
                                   const uint8_t *r, const uint8_t *s);

// Wipes memory that held a secret (not optimised away).
void rp_wipe(void *p, size_t n);

// Constant-time equality of two buffers.
bool rp_ct_equal(const uint8_t *a, const uint8_t *b, size_t n);

// ---- big numbers: Montgomery arithmetic with 32-bit limbs (src/crypto/bn.c) ----------------

enum { RP_BN_LIMBS = 136 };     // up to 4352 bits: RSA-4096 moduli with room

typedef struct {
    uint32_t n[RP_BN_LIMBS];    // odd modulus, little-endian limbs
    uint32_t rr[RP_BN_LIMBS];   // R^2 mod n, R = 2^(32k)
    size_t   k;                 // limbs in use
    size_t   bits;              // bit length of n
    uint32_t n0inv;             // -n^-1 mod 2^32
} rp_mont_t;

// An odd modulus as big-endian bytes (no more than RP_BN_LIMBS limbs); false otherwise.
bool rp_mont_init(rp_mont_t *m, const uint8_t *mod, size_t len);
// r = x mod n for a big-endian x of up to 2k limbs with x < n * R (e.g. x < n^2); r in k limbs.
void rp_mont_reduce_bytes(const rp_mont_t *m, const uint8_t *x, size_t len, uint32_t *r);
// r = base^exp mod n (normal form in and out, k limbs). `exp` big-endian; the time does not depend
// on the values of base or exp, only on the modulus size and exp_len.
void rp_mont_exp_ct(const rp_mont_t *m, uint32_t *r, const uint32_t *base, const uint8_t *exp, size_t exp_len);
// The same for a public exponent (may take less time for small exponents such as 65537).
void rp_mont_exp_pub(const rp_mont_t *m, uint32_t *r, const uint32_t *base, const uint8_t *exp, size_t exp_len);
// r = a * b mod n (normal form, k limbs).
void rp_mont_mulmod(const rp_mont_t *m, uint32_t *r, const uint32_t *a, const uint32_t *b);

// Montgomery-form building blocks (all constant time, k limbs, values below n):
// r = a b R^-1; r = a R (into Montgomery form); r = a R^-1 (out of it); r = a + b; r = a - b.
void rp_mont_mul(const rp_mont_t *m, uint32_t *r, const uint32_t *a, const uint32_t *b);
void rp_mont_to(const rp_mont_t *m, uint32_t *r, const uint32_t *a);
void rp_mont_from(const rp_mont_t *m, uint32_t *r, const uint32_t *a);
void rp_mont_add(const rp_mont_t *m, uint32_t *r, const uint32_t *a, const uint32_t *b);
void rp_mont_sub(const rp_mont_t *m, uint32_t *r, const uint32_t *a, const uint32_t *b);

// ---- RSA PKCS#1 v1.5 (RFC 8017 8.2) ------------------------------------------------------------

typedef struct {
    const uint8_t *n, *e, *d, *p, *q, *dp, *dq, *qinv;     // big-endian, no leading zero bytes needed
    size_t         n_len, e_len, d_len, p_len, q_len, dp_len, dq_len, qinv_len;
} rp_rsa_key_t;

// Signs a digest (EMSA-PKCS1-v1_5 with the DigestInfo of `alg`): CRT with blinding on both primes,
// constant-time exponentiation, and the signature is verified with (n, e) before it is returned.
// sig gets the modulus byte length. PROVEN_ERR_INVALID_ARG for a key that is not usable,
// PROVEN_ERR_INVALID_STATE when the check fails (a wrong key or a fault), PROVEN_ERR_IO when the OS
// random source fails.
[[nodiscard]] proven_err_t rp_rsa_sign(const rp_rsa_key_t *key, rp_hash_alg_t alg, const uint8_t *digest, uint8_t *sig,
                                       size_t *sig_len);
// Verifies a PKCS#1 v1.5 signature of a digest.
[[nodiscard]] bool rp_rsa_verify(const uint8_t *n, size_t n_len, const uint8_t *e, size_t e_len, rp_hash_alg_t alg,
                                 const uint8_t *digest, const uint8_t *sig, size_t sig_len);

#endif // RUBRAPACK_CRYPTO_H
