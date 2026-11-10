#ifndef PROVEN_SHA2_H
#define PROVEN_SHA2_H

/**
 * @file sha2.h
 * @brief SHA-256, SHA-384 and SHA-512 (FIPS 180-4) - proven_crypto (RFC-0045).
 *
 * Until v0.112 these were in the base library's `hash.h`, which keeps the non-cryptographic
 * hashes (FNV-1a, SipHash) and CRC-32. Byte-exact and platform-independent; checked against
 * FIPS 180-4's known-answer vectors. SHA-2 is published and unpatented.
 */

#include "proven/types.h"
#include "proven/memory.h"
// -----------------------------------------------------------------------------
// Cryptographic digest: for fingerprinting content
// -----------------------------------------------------------------------------

/** @brief Bytes in a SHA-256 digest. */
#define PROVEN_SHA256_SIZE ((proven_size_t)32)

/**
 * @brief A SHA-256 hashing context. Opaque; use the init/update/final calls.
 *
 * Its fields are exposed only so it can live on the stack with no allocation. Do not read or
 * write them - the layout is not part of the contract and the running state is meaningless
 * between update calls.
 */
typedef struct {
    proven_u32  state[8];
    proven_u64  length;      /* total bytes fed, for the length padding */
    proven_byte_t block[64]; /* the partial 64-byte block not yet compressed */
    proven_size_t block_len;
} proven_sha256_t;

/** @brief Begin a SHA-256 digest. */
void proven_sha256_init(proven_sha256_t *ctx);

/**
 * @brief Feed more bytes into a digest in progress. Call as many times as you like; the
 *        result depends only on the concatenation of everything fed, not on how it was split.
 */
void proven_sha256_update(proven_sha256_t *ctx, proven_mem_view_t data);

/**
 * @brief Finish the digest, writing 32 bytes to `out`. The context is spent afterwards;
 *        re-init it to hash something else.
 */
void proven_sha256_final(proven_sha256_t *ctx, proven_byte_t out[PROVEN_SHA256_SIZE]);

/**
 * @brief One-shot SHA-256 of a single buffer. Equivalent to init + one update + final.
 *
 * The cryptographic fingerprint the whole module exists to provide: content-addressing, dedup,
 * "are these two files identical" answered safely against a party trying to make two
 * different files look identical. This is what a content-addressed build cache or IR store
 * hashes with.
 */
void proven_sha256(proven_mem_view_t data, proven_byte_t out[PROVEN_SHA256_SIZE]);

/**
 * @brief Write a digest as 64 lowercase hex characters plus a NUL, into `out` (>= 65 bytes).
 *
 * A digest is bytes; a fingerprint people paste into logs, filenames, and URLs is hex. This
 * is the spelling everyone else uses (git, sha256sum), so it interoperates.
 */
void proven_sha256_to_hex(const proven_byte_t digest[PROVEN_SHA256_SIZE], char out[65]);

// -----------------------------------------------------------------------------
// SHA-512 and SHA-384
// -----------------------------------------------------------------------------

/** @brief Bytes in a SHA-512 digest. */
#define PROVEN_SHA512_SIZE ((proven_size_t)64)
/** @brief Bytes in a SHA-384 digest. */
#define PROVEN_SHA384_SIZE ((proven_size_t)48)

/**
 * @brief A running SHA-512 or SHA-384 digest (FIPS 180-4). Caller-owned state: it allocates
 *        nothing and there is nothing to destroy.
 *
 * The two are one algorithm with different starting values, SHA-384 keeping the first 48 bytes
 * of the result. Use them where a protocol names them - TLS cipher suites, certificate
 * signatures - or where a 64-bit machine should hash large inputs quickly: SHA-512 works in
 * 64-bit words and is the faster of the family there. For a plain content fingerprint,
 * proven_sha256 is the usual choice and the shorter digest.
 */
typedef struct {
    proven_u64 state[8];
    proven_u64 length;              /* bytes fed so far */
    proven_byte_t block[128];
    proven_size_t block_len;
} proven_sha512_t;

/** @brief The same state serves SHA-384. */
typedef proven_sha512_t proven_sha384_t;

/** @brief Begin a SHA-512 digest. */
void proven_sha512_init(proven_sha512_t *ctx);
/** @brief Feed more bytes. The result depends only on the bytes, not on how they were split. */
void proven_sha512_update(proven_sha512_t *ctx, proven_mem_view_t data);
/** @brief Finish, writing 64 bytes. The context is spent; re-init it to hash something else. */
void proven_sha512_final(proven_sha512_t *ctx, proven_byte_t out[PROVEN_SHA512_SIZE]);
/** @brief One-shot SHA-512 of a single buffer. */
void proven_sha512(proven_mem_view_t data, proven_byte_t out[PROVEN_SHA512_SIZE]);

/** @brief Begin a SHA-384 digest. */
void proven_sha384_init(proven_sha384_t *ctx);
/** @brief Feed more bytes into a SHA-384 digest. */
void proven_sha384_update(proven_sha384_t *ctx, proven_mem_view_t data);
/** @brief Finish, writing 48 bytes. A context begun with proven_sha384_init must be finished
 *         with this function, and one begun with proven_sha512_init with proven_sha512_final. */
void proven_sha384_final(proven_sha384_t *ctx, proven_byte_t out[PROVEN_SHA384_SIZE]);
/** @brief One-shot SHA-384 of a single buffer. */
void proven_sha384(proven_mem_view_t data, proven_byte_t out[PROVEN_SHA384_SIZE]);

/**
 * @brief One block of the hash's compression function over `state` (FIPS 180-4), with no
 *        padding and no length: the piece a constant-time MAC over secret-length data is built
 *        from (the legacy TLS records' "Lucky Thirteen" defence). Initial states are FIPS 180-4's.
 *        Not a hash by itself: for hashing use the functions above. Since v0.111.0.
 */
void proven_sha256_compress(proven_u32 state[8], const proven_byte_t block[64]);
/** @brief SHA-512's (and SHA-384's) compression function over one 128-byte block, likewise. */
void proven_sha512_compress(proven_u64 state[8], const proven_byte_t block[128]);

#endif /* PROVEN_SHA2_H */
