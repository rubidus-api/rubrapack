#ifndef PROVEN_AES_H
#define PROVEN_AES_H

#include "proven/types.h"
#include "proven/error.h"
#include "proven/memory.h"
#include "proven/aead.h"
#include "proven/random.h"

/**
 * @file aes.h
 * @brief AES-CBC (NIST SP 800-38A) over whole blocks, and PKCS #7 padding's removal - for reading
 *        what other tools encrypted: PBES2 in PKCS #8 key files, PKCS #12 (since v0.109.0). And
 *        (v0.117.0) proven_aes_rng_t, a cryptographic generator on AES-256 in counter mode.
 *
 * CBC is not authenticated: anyone can change the ciphertext and the plaintext changes with it.
 * Use it to open a file whose integrity something else checks (a key file's own structure, a
 * MAC), never as a new design - for that, the TLS engine's AEADs exist. The cipher is the one
 * the TLS engine uses: the processor's AES instructions where there are some, otherwise a
 * bitsliced one; neither takes a time that depends on the key or the data.
 *
 * Pure computation: no allocation, no OS; part of the freestanding profile.
 */

/**
 * @brief Decrypt `data` in place: whole 16-byte blocks under a 16-, 24- or 32-byte key.
 * @param iv the first IV; left holding the last ciphertext block, which is the next call's IV.
 * @return PROVEN_ERR_INVALID_ARG for another key length, a length that is not a multiple of 16,
 *         or null pointers. Nothing is changed on error.
 */
[[nodiscard]]
proven_err_t proven_aes_cbc_decrypt(proven_mem_view_t key, proven_byte_t iv[16], proven_mem_mut_t data);

/** @brief Encrypt `data` in place, the same way round. Errors as proven_aes_cbc_decrypt. */
[[nodiscard]]
proven_err_t proven_aes_cbc_encrypt(proven_mem_view_t key, proven_byte_t iv[16], proven_mem_mut_t data);

/**
 * @brief A CBC key prepared once, for many calls (records of the legacy TLS suites, say): the
 *        two calls above make and wipe one each time. The field is private; wipe it with
 *        proven_mem_wipe when done. Since v0.111.0.
 */
typedef struct {
    proven_aes_gcm_t k;
} proven_aes_cbc_key_t;

/** @brief Prepare a 16-, 24- or 32-byte key. PROVEN_ERR_INVALID_ARG otherwise. */
[[nodiscard]]
proven_err_t proven_aes_cbc_key_init(proven_aes_cbc_key_t *ctx, proven_mem_view_t key);

/** @brief proven_aes_cbc_decrypt under a prepared key. */
[[nodiscard]]
proven_err_t proven_aes_cbc_decrypt_with(const proven_aes_cbc_key_t *ctx, proven_byte_t iv[16], proven_mem_mut_t data);

/** @brief proven_aes_cbc_encrypt under a prepared key. */
[[nodiscard]]
proven_err_t proven_aes_cbc_encrypt_with(const proven_aes_cbc_key_t *ctx, proven_byte_t iv[16], proven_mem_mut_t data);

/**
 * @brief The length of `data` without its PKCS #7 padding (RFC 5652 section 6.3): 1 to `block`
 *        bytes, each the count. The check reads the whole last block whatever the padding says.
 * @return PROVEN_ERR_INVALID_FORMAT for padding that is not that (`*len` is then 0);
 *         PROVEN_ERR_INVALID_ARG for a `block` outside 1..255 or a length that is not a
 *         non-zero multiple of it.
 */
[[nodiscard]]
proven_err_t proven_pkcs7_unpad(proven_mem_view_t data, proven_size_t block, proven_size_t *len);

// -------------------------------------------------------------
// A cryptographic generator on AES-256 (RFC-0049)
// -------------------------------------------------------------

/**
 * @brief A cryptographic random generator: AES-256 in counter mode, under the same rules as
 *        random.h's proven_chacha_rng_t.
 *
 * Where the processor has AES instructions (AES-NI on x86, the ARMv8 extension on AArch64) this
 * is the fastest way here to make a lot of random bytes; without them it runs the bitsliced,
 * constant-time AES - the same bytes, many times slower, and proven_chacha_rng_t is then the
 * one to use. proven_aes_hw_available says which.
 *
 * The stream: the seed is an AES-256 key, and block j is AES(key, 0^96 || be32(j)) - the
 * standard AES-256-CTR keystream with a zero IV. **Key erasure** (unless
 * PROVEN_AES_RNG_NO_ERASURE): epochs of 64 blocks (1 KiB); blocks 0 to 61 are handed out (992
 * bytes), blocks 62 and 63 are the next epoch's key, whose counter starts at 0 again; bytes
 * handed out are wiped from the generator. So the first 992 bytes are AES-256-CTR's, and
 * someone who later reads the generator's memory can recompute at most what the current epoch
 * gave out. With NO_ERASURE it is plain AES-256-CTR throughout (a 96-bit big-endian counter in
 * the low bytes). Either way the stream is a function of the seed alone, whatever sizes it is
 * drawn in. **Fork:** one seeded from entropy takes a new key at its first draw in a forked
 * child (unless PROVEN_AES_RNG_NO_FORK_RESEED); **PROVEN_AES_RNG_RESEED_EVERY_MIB** mixes 32 bytes
 * from the entropy source into the key every 1 MiB of keystream. All as random.h says for
 * proven_chacha_rng_t - and "One seed, one use" there holds here too.
 *
 * Caller-owned: no allocation, nothing to destroy; copying one clones its stream.
 * The fields are private.
 */
typedef struct {
    proven_aes_t aes;          /* the current key, expanded */
    proven_byte_t block[128];  /* buffered keystream: up to eight blocks */
    proven_size_t used, have;  /* bytes of `block` handed out, and made */
    proven_u64 counter;        /* the next block: within the epoch with erasure, of the stream without */
    proven_u64 epoch;          /* epochs since the seed, for the reseed every MiB */
    proven_u32 seeded;         /* set only by seeding: a zeroed struct is inert */
} proven_aes_rng_t;

enum {
    PROVEN_AES_RNG_NO_ERASURE = PROVEN_CHACHA_RNG_NO_ERASURE,          /**< plain AES-256-CTR, nothing wiped */
    PROVEN_AES_RNG_NO_FORK_RESEED = PROVEN_CHACHA_RNG_NO_FORK_RESEED,  /**< seeded from entropy: a forked child replays the parent */
    PROVEN_AES_RNG_RESEED_EVERY_MIB = PROVEN_CHACHA_RNG_RESEED_EVERY_MIB, /**< seeded from entropy: new entropy into the key every MiB */
};

/** @brief Seed from 32 bytes of real entropy (the AES-256 key). Options 0: key erasure on. */
void proven_aes_rng_seed(proven_aes_rng_t *g, const proven_byte_t seed[32]);
/** @brief proven_aes_rng_seed with options; the two for a seed from entropy are ignored. */
void proven_aes_rng_seed_ex(proven_aes_rng_t *g, const proven_byte_t seed[32], unsigned options);
/** @brief Seed from random.h's entropy source. **Check this:** on false the generator is inert -
 *         zeros and an invalid trait. */
[[nodiscard]]
bool proven_aes_rng_seed_from_entropy(proven_aes_rng_t *g);
/** @brief proven_aes_rng_seed_from_entropy with options (PROVEN_AES_RNG_*). */
[[nodiscard]]
bool proven_aes_rng_seed_from_entropy_ex(proven_aes_rng_t *g, unsigned options);
/** @brief The next 64-bit word (little-endian from the stream); 0 for an unseeded generator. */
proven_u64 proven_aes_rng_next(proven_aes_rng_t *g);
/** @brief Fill `len` bytes; cannot fail once seeded. Zeros from an unseeded generator. */
void proven_aes_rng_fill(proven_aes_rng_t *g, void *buf, proven_size_t len);
/** @brief View it as a proven_rng_t, for random.h's helpers: invalid if it was never seeded. */
[[nodiscard]]
proven_rng_t proven_aes_rng(proven_aes_rng_t *g);

#endif /* PROVEN_AES_H */
