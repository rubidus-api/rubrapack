#include "proven/sha2.h"

// =============================================================================
// SHA-256 (FIPS 180-4) - cryptographic digest
// =============================================================================

static const proven_u32 sha256_k[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static proven_u32 sha_rotr(proven_u32 x, int n) {
    return (x >> n) | (x << (32 - n));
}

static void sha256_compress(proven_u32 state[8], const proven_byte_t block[64]) {
    proven_u32 w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = ((proven_u32)block[i * 4] << 24) | ((proven_u32)block[i * 4 + 1] << 16) |
               ((proven_u32)block[i * 4 + 2] << 8) | (proven_u32)block[i * 4 + 3];
    }
    for (int i = 16; i < 64; ++i) {
        proven_u32 s0 = sha_rotr(w[i - 15], 7) ^ sha_rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        proven_u32 s1 = sha_rotr(w[i - 2], 17) ^ sha_rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    proven_u32 a = state[0], b = state[1], c = state[2], d = state[3];
    proven_u32 e = state[4], f = state[5], g = state[6], h = state[7];

    for (int i = 0; i < 64; ++i) {
        proven_u32 S1 = sha_rotr(e, 6) ^ sha_rotr(e, 11) ^ sha_rotr(e, 25);
        proven_u32 ch = (e & f) ^ (~e & g);
        proven_u32 t1 = h + S1 + ch + sha256_k[i] + w[i];
        proven_u32 S0 = sha_rotr(a, 2) ^ sha_rotr(a, 13) ^ sha_rotr(a, 22);
        proven_u32 maj = (a & b) ^ (a & c) ^ (b & c);
        proven_u32 t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }

    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

/* The block function by itself, for the legacy TLS unit (see hash_legacy.c). */
void proven_sha256_compress(proven_u32 state[8], const proven_byte_t block[64]) { sha256_compress(state, block); }

void proven_sha256_init(proven_sha256_t *ctx) {
    if (!ctx) return;
    ctx->state[0] = 0x6a09e667u; ctx->state[1] = 0xbb67ae85u;
    ctx->state[2] = 0x3c6ef372u; ctx->state[3] = 0xa54ff53au;
    ctx->state[4] = 0x510e527fu; ctx->state[5] = 0x9b05688cu;
    ctx->state[6] = 0x1f83d9abu; ctx->state[7] = 0x5be0cd19u;
    ctx->length = 0;
    ctx->block_len = 0;
}

void proven_sha256_update(proven_sha256_t *ctx, proven_mem_view_t data) {
    /* An empty view - {NULL, 0} included - changes nothing; returning here also keeps `p` from
     * being advanced from NULL below (pointer arithmetic on NULL, which UBSan reports). */
    if (!ctx || data.size == 0 || !data.ptr) return;
    ctx->length += data.size;
    const proven_byte_t *p = data.ptr;
    proven_size_t n = data.size;
    /* Top up a partly filled block first; then compress whole blocks straight from the input,
     * with no copy (RFC-0009 P-107 - it used to copy every byte into the block); keep the tail. */
    if (ctx->block_len > 0) {
        proven_size_t take = 64u - ctx->block_len;
        if (take > n) take = n;
        for (proven_size_t i = 0; i < take; ++i) ctx->block[ctx->block_len + i] = p[i];
        ctx->block_len += take;
        p += take;
        n -= take;
        if (ctx->block_len < 64u) return;
        sha256_compress(ctx->state, ctx->block);
        ctx->block_len = 0;
    }
    while (n >= 64u) {
        sha256_compress(ctx->state, p);
        p += 64;
        n -= 64;
    }
    for (proven_size_t i = 0; i < n; ++i) ctx->block[i] = p[i];
    ctx->block_len = n;
}

void proven_sha256_final(proven_sha256_t *ctx, proven_byte_t out[PROVEN_SHA256_SIZE]) {
    if (!ctx || !out) return;

    /* The length in BITS, captured before the padding is appended. */
    proven_u64 bits = ctx->length * 8u;

    /* Append 0x80, then zeros, until the block is 56 bytes - leaving 8 for the length. If
     * there is not room, finish this block first and pad in a fresh one (the case the
     * 56-byte and million-'a' vectors exist to catch). */
    proven_byte_t one = 0x80;
    proven_sha256_update(ctx, (proven_mem_view_t){ .ptr = &one, .size = 1 });
    /* proven_sha256_update bumped length; it does not matter, `bits` was already taken. */
    proven_byte_t zero = 0;
    while (ctx->block_len != 56) {
        proven_sha256_update(ctx, (proven_mem_view_t){ .ptr = &zero, .size = 1 });
    }

    proven_byte_t lenbytes[8];
    for (int i = 0; i < 8; ++i) lenbytes[i] = (proven_byte_t)(bits >> (56 - 8 * i));
    proven_sha256_update(ctx, (proven_mem_view_t){ .ptr = lenbytes, .size = 8 });
    /* block_len is now 0: the last block was compressed by the final update. */

    for (int i = 0; i < 8; ++i) {
        out[i * 4]     = (proven_byte_t)(ctx->state[i] >> 24);
        out[i * 4 + 1] = (proven_byte_t)(ctx->state[i] >> 16);
        out[i * 4 + 2] = (proven_byte_t)(ctx->state[i] >> 8);
        out[i * 4 + 3] = (proven_byte_t)(ctx->state[i]);
    }
}

void proven_sha256(proven_mem_view_t data, proven_byte_t out[PROVEN_SHA256_SIZE]) {
    proven_sha256_t ctx;
    proven_sha256_init(&ctx);
    proven_sha256_update(&ctx, data);
    proven_sha256_final(&ctx, out);
}

void proven_sha256_to_hex(const proven_byte_t digest[PROVEN_SHA256_SIZE], char out[65]) {
    static const char d[] = "0123456789abcdef";
    if (!digest || !out) return;
    for (proven_size_t i = 0; i < PROVEN_SHA256_SIZE; ++i) {
        out[i * 2]     = d[digest[i] >> 4];
        out[i * 2 + 1] = d[digest[i] & 0xf];
    }
    out[64] = 0;
}

// -----------------------------------------------------------------------------
// SHA-512 and SHA-384 (FIPS 180-4)
// -----------------------------------------------------------------------------

static const proven_u64 sha512_k[80] = {
    0x428a2f98d728ae22ull, 0x7137449123ef65cdull, 0xb5c0fbcfec4d3b2full, 0xe9b5dba58189dbbcull,
    0x3956c25bf348b538ull, 0x59f111f1b605d019ull, 0x923f82a4af194f9bull, 0xab1c5ed5da6d8118ull,
    0xd807aa98a3030242ull, 0x12835b0145706fbeull, 0x243185be4ee4b28cull, 0x550c7dc3d5ffb4e2ull,
    0x72be5d74f27b896full, 0x80deb1fe3b1696b1ull, 0x9bdc06a725c71235ull, 0xc19bf174cf692694ull,
    0xe49b69c19ef14ad2ull, 0xefbe4786384f25e3ull, 0x0fc19dc68b8cd5b5ull, 0x240ca1cc77ac9c65ull,
    0x2de92c6f592b0275ull, 0x4a7484aa6ea6e483ull, 0x5cb0a9dcbd41fbd4ull, 0x76f988da831153b5ull,
    0x983e5152ee66dfabull, 0xa831c66d2db43210ull, 0xb00327c898fb213full, 0xbf597fc7beef0ee4ull,
    0xc6e00bf33da88fc2ull, 0xd5a79147930aa725ull, 0x06ca6351e003826full, 0x142929670a0e6e70ull,
    0x27b70a8546d22ffcull, 0x2e1b21385c26c926ull, 0x4d2c6dfc5ac42aedull, 0x53380d139d95b3dfull,
    0x650a73548baf63deull, 0x766a0abb3c77b2a8ull, 0x81c2c92e47edaee6ull, 0x92722c851482353bull,
    0xa2bfe8a14cf10364ull, 0xa81a664bbc423001ull, 0xc24b8b70d0f89791ull, 0xc76c51a30654be30ull,
    0xd192e819d6ef5218ull, 0xd69906245565a910ull, 0xf40e35855771202aull, 0x106aa07032bbd1b8ull,
    0x19a4c116b8d2d0c8ull, 0x1e376c085141ab53ull, 0x2748774cdf8eeb99ull, 0x34b0bcb5e19b48a8ull,
    0x391c0cb3c5c95a63ull, 0x4ed8aa4ae3418acbull, 0x5b9cca4f7763e373ull, 0x682e6ff3d6b2b8a3ull,
    0x748f82ee5defb2fcull, 0x78a5636f43172f60ull, 0x84c87814a1f0ab72ull, 0x8cc702081a6439ecull,
    0x90befffa23631e28ull, 0xa4506cebde82bde9ull, 0xbef9a3f7b2c67915ull, 0xc67178f2e372532bull,
    0xca273eceea26619cull, 0xd186b8c721c0c207ull, 0xeada7dd6cde0eb1eull, 0xf57d4f7fee6ed178ull,
    0x06f067aa72176fbaull, 0x0a637dc5a2c898a6ull, 0x113f9804bef90daeull, 0x1b710b35131c471bull,
    0x28db77f523047d84ull, 0x32caab7b40c72493ull, 0x3c9ebe0a15c9bebcull, 0x431d67c49c100d4cull,
    0x4cc5d4becb3e42b6ull, 0x597f299cfc657e2aull, 0x5fcb6fab3ad6faecull, 0x6c44198c4a475817ull
};

#define SHA512_ROTR(x, n) (((x) >> (n)) | ((x) << (64 - (n))))

static void sha512_compress(proven_u64 state[8], const proven_byte_t block[128]) {
    proven_u64 w[80];
    for (int i = 0; i < 16; ++i) {
        proven_u64 v = 0;
        for (int k = 0; k < 8; ++k) v = (v << 8) | (proven_u64)block[i * 8 + k];
        w[i] = v;
    }
    for (int i = 16; i < 80; ++i) {
        proven_u64 s0 = SHA512_ROTR(w[i - 15], 1) ^ SHA512_ROTR(w[i - 15], 8) ^ (w[i - 15] >> 7);
        proven_u64 s1 = SHA512_ROTR(w[i - 2], 19) ^ SHA512_ROTR(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    proven_u64 a = state[0], b = state[1], c = state[2], d = state[3];
    proven_u64 e = state[4], f = state[5], g = state[6], h = state[7];
    for (int i = 0; i < 80; ++i) {
        proven_u64 S1 = SHA512_ROTR(e, 14) ^ SHA512_ROTR(e, 18) ^ SHA512_ROTR(e, 41);
        proven_u64 ch = (e & f) ^ (~e & g);
        proven_u64 t1 = h + S1 + ch + sha512_k[i] + w[i];
        proven_u64 S0 = SHA512_ROTR(a, 28) ^ SHA512_ROTR(a, 34) ^ SHA512_ROTR(a, 39);
        proven_u64 maj = (a & b) ^ (a & c) ^ (b & c);
        proven_u64 t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

/* The block function by itself, for the legacy TLS unit (see hash_legacy.c). */
void proven_sha512_compress(proven_u64 state[8], const proven_byte_t block[128]) { sha512_compress(state, block); }

void proven_sha512_init(proven_sha512_t *ctx) {
    if (!ctx) return;
    ctx->state[0] = 0x6a09e667f3bcc908ull; ctx->state[1] = 0xbb67ae8584caa73bull;
    ctx->state[2] = 0x3c6ef372fe94f82bull; ctx->state[3] = 0xa54ff53a5f1d36f1ull;
    ctx->state[4] = 0x510e527fade682d1ull; ctx->state[5] = 0x9b05688c2b3e6c1full;
    ctx->state[6] = 0x1f83d9abfb41bd6bull; ctx->state[7] = 0x5be0cd19137e2179ull;
    ctx->length = 0;
    ctx->block_len = 0;
}

void proven_sha384_init(proven_sha384_t *ctx) {
    if (!ctx) return;
    ctx->state[0] = 0xcbbb9d5dc1059ed8ull; ctx->state[1] = 0x629a292a367cd507ull;
    ctx->state[2] = 0x9159015a3070dd17ull; ctx->state[3] = 0x152fecd8f70e5939ull;
    ctx->state[4] = 0x67332667ffc00b31ull; ctx->state[5] = 0x8eb44a8768581511ull;
    ctx->state[6] = 0xdb0c2e0d64f98fa7ull; ctx->state[7] = 0x47b5481dbefa4fa4ull;
    ctx->length = 0;
    ctx->block_len = 0;
}

void proven_sha512_update(proven_sha512_t *ctx, proven_mem_view_t data) {
    if (!ctx || !data.ptr || data.size == 0) return;
    const proven_byte_t *p = data.ptr;
    proven_size_t n = data.size;
    ctx->length += (proven_u64)n;
    if (ctx->block_len > 0) {
        while (n > 0 && ctx->block_len < 128) { ctx->block[ctx->block_len++] = *p++; n--; }
        if (ctx->block_len < 128) return;
        sha512_compress(ctx->state, ctx->block);
        ctx->block_len = 0;
    }
    while (n >= 128) {
        sha512_compress(ctx->state, p);
        p += 128;
        n -= 128;
    }
    while (n > 0) { ctx->block[ctx->block_len++] = *p++; n--; }
}

void proven_sha384_update(proven_sha384_t *ctx, proven_mem_view_t data) {
    proven_sha512_update(ctx, data);
}

/* Pad and write the first `out_len` bytes of the chain value. */
static void sha512_finish(proven_sha512_t *ctx, proven_byte_t *out, proven_size_t out_len) {
    /* The length is a 128-bit count of bits; a byte count in 64 bits gives at most 67 of them. */
    proven_u64 bits_hi = ctx->length >> 61;
    proven_u64 bits_lo = ctx->length << 3;
    ctx->block[ctx->block_len++] = 0x80;
    if (ctx->block_len > 112) {
        while (ctx->block_len < 128) ctx->block[ctx->block_len++] = 0;
        sha512_compress(ctx->state, ctx->block);
        ctx->block_len = 0;
    }
    while (ctx->block_len < 112) ctx->block[ctx->block_len++] = 0;
    for (int i = 0; i < 8; ++i) ctx->block[112 + i] = (proven_byte_t)(bits_hi >> (56 - 8 * i));
    for (int i = 0; i < 8; ++i) ctx->block[120 + i] = (proven_byte_t)(bits_lo >> (56 - 8 * i));
    sha512_compress(ctx->state, ctx->block);
    for (proven_size_t i = 0; i < out_len; ++i) out[i] = (proven_byte_t)(ctx->state[i / 8] >> (56 - 8 * (i % 8)));
    ctx->block_len = 0;
}

void proven_sha512_final(proven_sha512_t *ctx, proven_byte_t out[PROVEN_SHA512_SIZE]) {
    if (!ctx || !out) return;
    sha512_finish(ctx, out, PROVEN_SHA512_SIZE);
}

void proven_sha384_final(proven_sha384_t *ctx, proven_byte_t out[PROVEN_SHA384_SIZE]) {
    if (!ctx || !out) return;
    sha512_finish(ctx, out, PROVEN_SHA384_SIZE);
}

void proven_sha512(proven_mem_view_t data, proven_byte_t out[PROVEN_SHA512_SIZE]) {
    proven_sha512_t ctx;
    proven_sha512_init(&ctx);
    proven_sha512_update(&ctx, data);
    proven_sha512_final(&ctx, out);
}

void proven_sha384(proven_mem_view_t data, proven_byte_t out[PROVEN_SHA384_SIZE]) {
    proven_sha384_t ctx;
    proven_sha384_init(&ctx);
    proven_sha384_update(&ctx, data);
    proven_sha384_final(&ctx, out);
}
