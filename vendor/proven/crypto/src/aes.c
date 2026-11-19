#include "proven/aes.h"
#include "proven_internal_crypto.h"

/* aes.h: CBC over the TLS engine's AES, and PKCS #7 padding. */

static proven_err_t aes_cbc(proven_mem_view_t key, proven_byte_t iv[16], proven_mem_mut_t data, bool decrypt) {
    if (!key.ptr || !iv || (data.size > 0 && !data.ptr) || data.size % 16 != 0) return PROVEN_ERR_INVALID_ARG;
    proven_crypto_aes_gcm_t ctx;
    if (!proven_crypto_aes_cbc_init(&ctx, key.ptr, key.size)) return PROVEN_ERR_INVALID_ARG;
    if (decrypt) proven_crypto_aes_cbc_decrypt(&ctx, iv, data.ptr, data.size);
    else proven_crypto_aes_cbc_encrypt(&ctx, iv, data.ptr, data.size);
    proven_mem_wipe((proven_mem_mut_t){ .ptr = (proven_byte_t *)&ctx, .size = sizeof ctx });
    return PROVEN_OK;
}

proven_err_t proven_aes_cbc_decrypt(proven_mem_view_t key, proven_byte_t iv[16], proven_mem_mut_t data) {
    return aes_cbc(key, iv, data, true);
}

proven_err_t proven_aes_cbc_encrypt(proven_mem_view_t key, proven_byte_t iv[16], proven_mem_mut_t data) {
    return aes_cbc(key, iv, data, false);
}

proven_err_t proven_aes_cbc_key_init(proven_aes_cbc_key_t *ctx, proven_mem_view_t key) {
    if (!ctx || !key.ptr) return PROVEN_ERR_INVALID_ARG;
    return proven_crypto_aes_cbc_init(&ctx->k, key.ptr, key.size) ? PROVEN_OK : PROVEN_ERR_INVALID_ARG;
}

static proven_err_t aes_cbc_with(const proven_aes_cbc_key_t *ctx, proven_byte_t iv[16], proven_mem_mut_t data, bool decrypt) {
    if (!ctx || !iv || (data.size > 0 && !data.ptr) || data.size % 16 != 0) return PROVEN_ERR_INVALID_ARG;
    if (decrypt) proven_crypto_aes_cbc_decrypt(&ctx->k, iv, data.ptr, data.size);
    else proven_crypto_aes_cbc_encrypt(&ctx->k, iv, data.ptr, data.size);
    return PROVEN_OK;
}

proven_err_t proven_aes_cbc_decrypt_with(const proven_aes_cbc_key_t *ctx, proven_byte_t iv[16], proven_mem_mut_t data) {
    return aes_cbc_with(ctx, iv, data, true);
}

proven_err_t proven_aes_cbc_encrypt_with(const proven_aes_cbc_key_t *ctx, proven_byte_t iv[16], proven_mem_mut_t data) {
    return aes_cbc_with(ctx, iv, data, false);
}

proven_err_t proven_pkcs7_unpad(proven_mem_view_t data, proven_size_t block, proven_size_t *len) {
    if (len) *len = 0;
    if (!len || block == 0 || block > 255 || !data.ptr || data.size == 0 || data.size % block != 0) return PROVEN_ERR_INVALID_ARG;
    /* Every byte of the last block is looked at, and the verdict gathered without a branch on
     * it, so how far the check got does not show in its time. */
    const proven_byte_t *last = data.ptr + data.size - block;
    const proven_u32 pad = data.ptr[data.size - 1];
    proven_u32 bad = (proven_u32)((pad - 1u) >> 31) | (proven_u32)((block - (proven_size_t)pad) >> (sizeof(proven_size_t) * 8 - 1));
    for (proven_size_t i = 0; i < block; ++i) {
        const proven_u32 in_pad = (proven_u32)(((proven_u32)(block - 1 - i) - pad) >> 31);   /* 1 for the last `pad` bytes */
        bad |= in_pad & (proven_u32)(last[i] != pad);
    }
    if (bad) return PROVEN_ERR_INVALID_FORMAT;
    *len = data.size - pad;
    return PROVEN_OK;
}

// -------------------------------------------------------------
// proven_aes_rng_t (RFC-0049): AES-256-CTR under the ChaCha generator's rules
// -------------------------------------------------------------

#define AES_RNG_MAGIC 0xAE5Eu
#define AES_RNG_PLAIN (1u << 16)           /* PROVEN_AES_RNG_NO_ERASURE */
#define AES_RNG_FORK (1u << 17)            /* seeded from entropy: a new key in a forked child */
#define AES_RNG_MIB (1u << 18)             /* PROVEN_AES_RNG_RESEED_EVERY_MIB */
#define AES_RNG_GEN_SHIFT 20
#define AES_RNG_GEN_MASK 0xFFFu
#define AES_RNG_DATA 62u                   /* blocks handed out in an epoch; 62 and 63 are the next key */
#define AES_RNG_MIB_EPOCHS 1024u           /* 1 MiB of keystream */

static bool aes_rng_ok(const proven_aes_rng_t *g) { return (g->seeded & 0xFFFFu) == AES_RNG_MAGIC; }

static void aes_rng_wipe(void *p, proven_size_t n) {
#if (defined(__GNUC__) || defined(__clang__)) && !defined(PROVEN_FREESTANDING)
    __builtin_memset(p, 0, n);                                      /* wide stores, kept by the barrier */
    __asm__ __volatile__("" : : "r"(p) : "memory");
#else
    proven_mem_wipe((proven_mem_mut_t){ .ptr = (proven_byte_t *)p, .size = n });
#endif
}

static void aes_rng_copy(proven_byte_t *to, const proven_byte_t *from, proven_size_t n) {
#if (defined(__GNUC__) || defined(__clang__)) && !defined(PROVEN_FREESTANDING)
    __builtin_memcpy(to, from, n);
#else
    for (proven_size_t i = 0; i < n; ++i) to[i] = from[i];
#endif
}

/* Zeros, for counter mode to XOR the keystream onto: a call never makes more than 64 blocks. */
static const proven_byte_t g_aes_rng_zero[1024];

/* Write the keystream of `blocks` blocks (at most 64) from `counter` on to `out`. The counter's
 * high bits go into the nonce (a 96-bit big-endian count), so no call crosses the 32-bit
 * counter's wrap. */
static void aes_rng_keystream(const proven_aes_rng_t *g, proven_u64 counter, proven_byte_t *out, proven_size_t blocks) {
    while (blocks > 0) {
        const proven_u32 lo = (proven_u32)counter;
        const proven_u64 hi = counter >> 32;
        proven_size_t n = blocks;
        const proven_u64 to_wrap = (proven_u64)0x100000000ull - lo;
        if ((proven_u64)n > to_wrap) n = (proven_size_t)to_wrap;
        proven_byte_t nonce[12] = { 0, 0, 0, 0, (proven_byte_t)(hi >> 56), (proven_byte_t)(hi >> 48), (proven_byte_t)(hi >> 40), (proven_byte_t)(hi >> 32),
                                    (proven_byte_t)(hi >> 24), (proven_byte_t)(hi >> 16), (proven_byte_t)(hi >> 8), (proven_byte_t)hi };
        proven_crypto_aes_ctr(&g->aes, nonce, lo, g_aes_rng_zero, out, 16 * n);
        out += 16 * n;
        blocks -= n;
        counter += n;
    }
}

/* The epoch's end: `k` (blocks 62 and 63) is the next key; the counter starts again. */
static void aes_rng_rekey_with(proven_aes_rng_t *g, proven_byte_t k[32]) {
    g->epoch++;
    if ((g->seeded & AES_RNG_MIB) && g->epoch % AES_RNG_MIB_EPOCHS == 0) {
        proven_byte_t e[32];
        if (proven_random_bytes(e, sizeof e)) for (int i = 0; i < 32; ++i) k[i] ^= e[i];
        aes_rng_wipe(e, sizeof e);
    }
    (void)proven_crypto_aes_init(&g->aes, k, 32);         /* the old round keys are overwritten */
    aes_rng_wipe(k, 32);
    g->counter = 0;
}

/* Up to `want` blocks of the stream into `out`; returns how many (at least one). With erasure,
 * never past the epoch's data; its last eight blocks (56 to 63, the key among them) are made in
 * one call, so the processor keeps eight in flight, and the key is taken from them. */
static proven_size_t aes_rng_blocks(proven_aes_rng_t *g, proven_byte_t *out, proven_size_t want) {
    if (want > 64) want = 64;
    if (g->seeded & AES_RNG_PLAIN) {
        aes_rng_keystream(g, g->counter, out, want);
        g->counter += want;
        return want;
    }
    const proven_size_t tail = AES_RNG_DATA - 6;                  /* block 56 */
    if (g->counter < tail) {
        proven_size_t n = (proven_size_t)(tail - g->counter);
        if (n > want) n = want;
        aes_rng_keystream(g, g->counter, out, n);
        g->counter += n;
        return n;
    }
    proven_byte_t t[128];
    aes_rng_keystream(g, tail, t, 8);
    proven_size_t n = (proven_size_t)(AES_RNG_DATA - g->counter);
    if (n > want) n = want;
    const proven_size_t from = 16 * (proven_size_t)(g->counter - tail);
    aes_rng_copy(out, t + from, 16 * n);
    g->counter += n;
    if (g->counter == AES_RNG_DATA) aes_rng_rekey_with(g, t + 96);
    aes_rng_wipe(t, sizeof t);
    return n;
}

static void aes_rng_fork_check(proven_aes_rng_t *g) {
    if (!(g->seeded & AES_RNG_FORK)) return;
    const proven_u32 gen = (proven_u32)proven_random_fork_generation() & AES_RNG_GEN_MASK;
    if (gen == g->seeded >> AES_RNG_GEN_SHIFT) return;
    /* In a forked child: a new key from the entropy source. If that fails (a draw cannot), the
     * thread's own ChaCha generator, if it exists - which in this child has taken a key of its
     * own, from its old one and the process id; failing that, one derived from the old key and
     * the fork generation. Either is different from the parent's stream, not secret from it; the
     * last does not tell two children of one parent apart. */
    proven_byte_t k[32];
    if (!proven_random_bytes(k, sizeof k) && !proven_random_fast_bytes(k, sizeof k)) {
        proven_byte_t nonce[12] = { 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0, (proven_byte_t)(gen >> 24), (proven_byte_t)(gen >> 16), (proven_byte_t)(gen >> 8), (proven_byte_t)gen };
        for (int i = 0; i < 32; ++i) k[i] = 0;
        proven_crypto_aes_ctr(&g->aes, nonce, 0, k, k, sizeof k);
    }
    (void)proven_crypto_aes_init(&g->aes, k, sizeof k);
    aes_rng_wipe(k, sizeof k);
    aes_rng_wipe(g->block, sizeof g->block);
    g->used = g->have = 0;
    g->counter = 0;
    g->epoch = 0;
    g->seeded = (g->seeded & ((1u << AES_RNG_GEN_SHIFT) - 1u)) | (gen << AES_RNG_GEN_SHIFT);
}

void proven_aes_rng_seed_ex(proven_aes_rng_t *g, const proven_byte_t seed[32], unsigned options) {
    if (!g || !seed) return;
    (void)proven_crypto_aes_init(&g->aes, seed, 32);
    aes_rng_wipe(g->block, sizeof g->block);
    g->used = g->have = 0;
    g->counter = 0;
    g->epoch = 0;
    g->seeded = AES_RNG_MAGIC | ((options & PROVEN_AES_RNG_NO_ERASURE) ? AES_RNG_PLAIN : 0u);
}

void proven_aes_rng_seed(proven_aes_rng_t *g, const proven_byte_t seed[32]) { proven_aes_rng_seed_ex(g, seed, 0u); }

bool proven_aes_rng_seed_from_entropy_ex(proven_aes_rng_t *g, unsigned options) {
    if (!g) return false;
    proven_byte_t seed[32];
    if (!proven_random_bytes(seed, sizeof seed)) {
        aes_rng_wipe(g, sizeof *g);                   /* inert: `seeded` is 0 */
        return false;
    }
    proven_aes_rng_seed_ex(g, seed, options);
    aes_rng_wipe(seed, sizeof seed);
    if (!(options & PROVEN_AES_RNG_NO_FORK_RESEED)) g->seeded |= AES_RNG_FORK | (((proven_u32)proven_random_fork_generation() & AES_RNG_GEN_MASK) << AES_RNG_GEN_SHIFT);
    if ((options & PROVEN_AES_RNG_RESEED_EVERY_MIB) && !(options & PROVEN_AES_RNG_NO_ERASURE)) g->seeded |= AES_RNG_MIB;
    return true;
}

bool proven_aes_rng_seed_from_entropy(proven_aes_rng_t *g) { return proven_aes_rng_seed_from_entropy_ex(g, 0u); }

void proven_aes_rng_fill(proven_aes_rng_t *g, void *buf, proven_size_t len) {
    if (!g || !buf || len == 0) return;
    proven_byte_t *p = (proven_byte_t *)buf;
    if (!aes_rng_ok(g)) {
        for (proven_size_t i = 0; i < len; ++i) p[i] = 0;
        return;
    }
    aes_rng_fork_check(g);
    const bool plain = (g->seeded & AES_RNG_PLAIN) != 0;
    /* What is buffered first; with erasure, what is handed out is wiped. */
    if (g->used < g->have) {
        proven_size_t take = g->have - g->used;
        if (take > len) take = len;
        aes_rng_copy(p, g->block + g->used, take);
        if (!plain) aes_rng_wipe(g->block + g->used, take);
        g->used += take;
        p += take;
        len -= take;
        if (len == 0) return;
    }
    /* Whole blocks straight into the buffer - eight at once on AES-NI - an epoch's data at a time. */
    while (len >= 16) {
        const proven_size_t n = aes_rng_blocks(g, p, len / 16);
        p += 16 * n;
        len -= 16 * n;
    }
    /* The tail through the buffer: up to eight blocks, never past the epoch's end. */
    if (len > 0) {
        const proven_size_t n = aes_rng_blocks(g, g->block, 8);
        g->have = 16 * n;
        aes_rng_copy(p, g->block, len);
        if (!plain) aes_rng_wipe(g->block, len);
        g->used = len;
    }
}

proven_u64 proven_aes_rng_next(proven_aes_rng_t *g) {
    if (!g || !aes_rng_ok(g)) return 0;
    if (g->have - g->used >= 8 && !(g->seeded & AES_RNG_FORK && ((proven_u32)proven_random_fork_generation() & AES_RNG_GEN_MASK) != g->seeded >> AES_RNG_GEN_SHIFT)) {
        proven_byte_t *q = g->block + g->used;                      /* the common case: eight bytes buffered */
        proven_u64 v = 0;
        for (int i = 0; i < 8; ++i) v |= (proven_u64)q[i] << (8 * i);
        if (!(g->seeded & AES_RNG_PLAIN)) aes_rng_wipe(q, 8);
        g->used += 8;
        return v;
    }
    proven_byte_t b[8];
    proven_aes_rng_fill(g, b, sizeof b);
    proven_u64 v = 0;
    for (int i = 0; i < 8; ++i) v |= (proven_u64)b[i] << (8 * i);
    aes_rng_wipe(b, sizeof b);
    return v;
}

static proven_u64 aes_rng_next_ctx(void *ctx) { return proven_aes_rng_next((proven_aes_rng_t *)ctx); }
static void aes_rng_fill_ctx(void *ctx, void *buf, proven_size_t len) { proven_aes_rng_fill((proven_aes_rng_t *)ctx, buf, len); }
static const proven_rng_vtable_t g_aes_rng_vt = { .next_u64 = aes_rng_next_ctx, .fill = aes_rng_fill_ctx };

proven_rng_t proven_aes_rng(proven_aes_rng_t *g) {
    if (!g || !aes_rng_ok(g)) return (proven_rng_t){ 0 };
    return (proven_rng_t){ .vt = &g_aes_rng_vt, .ctx = g };
}
