// src/codec/lzx.c - LZX encoder and decoder for cabinet folders (include/rubrapack/lzx.h).
//
// The bit stream is a sequence of 16-bit little-endian words whose bits are used from the most
// significant down. A folder starts with one bit (1 = E8 translation, then the 32-bit translation
// size). Blocks follow: 3 bits of type (1 verbatim, 2 aligned offset, 3 uncompressed) and 24 bits
// of uncompressed size, then for types 1 and 2 the code lengths of the main tree (256 literals, then
// 8 symbols per position slot) and of the length tree (249 symbols), each range sent as differences
// from the previous block's lengths through a 20-symbol pretree. A main symbol above 255 is a match:
// its low 3 bits the length - 2 (7: add a length-tree symbol), the rest the position slot (0-2 the
// three repeated offsets). After every 32768 output bytes the stream is aligned to 16 bits.

#include "rubrapack/lzx.h"
#include "rubrapack/mem.h"

#include <string.h>

enum {
    MIN_MATCH = 2,
    MAX_MATCH = 257,
    PRIMARY = 7,
    SECONDARY = 249,
    PRETREE = 20,
    ALIGNED = 8,
    MAX_SLOTS = 50,
    MAX_MAIN = 256 + MAX_SLOTS * 8,
    TYPE_VERBATIM = 1,
    TYPE_ALIGNED = 2,
    TYPE_UNCOMPRESSED = 3,
    HASH_BITS = 16,
    CHAIN = 64,             // positions tried per match search (128: 1% smaller, 40% slower)
    NICE = 96,              // a match this long ends the search
};

static const uint8_t slots_for[7] = { 30, 32, 34, 36, 38, 42, 50 };      // window 2^15 .. 2^21

// Extra offset bits and the first formatted offset of each position slot.
static const uint8_t extra_tab[MAX_SLOTS + 1] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11,
    12, 12, 13, 13, 14, 14, 15, 15, 16, 16, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17,
};
static const uint32_t base_tab[MAX_SLOTS + 1] = {
    0, 1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024, 1536, 2048, 3072,
    4096, 6144, 8192, 12288, 16384, 24576, 32768, 49152, 65536, 98304, 131072, 196608, 262144, 393216,
    524288, 655360, 786432, 917504, 1048576, 1179648, 1310720, 1441792, 1572864, 1703936, 1835008,
    1966080, 2097152,
};

static uint8_t extra_bits(int slot) { return extra_tab[slot]; }
static uint32_t slot_base(int slot) { return base_tab[slot]; }

// ---- Huffman lengths (package-merge, limited) and canonical codes ----------------------------

typedef struct {
    uint64_t w;
    int32_t  leaf, a, b;
} pm_item_t;

static void pm_count(pm_item_t *const *lists, int level, int32_t idx, uint8_t *lens) {
    const pm_item_t *it = &lists[level][idx];
    if (it->leaf >= 0) {
        lens[it->leaf]++;
        return;
    }
    pm_count(lists, level - 1, it->a, lens);
    pm_count(lists, level - 1, it->b, lens);
}

// Lengths <= limit (<= 16) for freq[0..n); unused symbols get 0; at least two symbols get a length,
// so the code is always complete.
static bool build_lengths(proven_allocator_t alloc, const uint32_t *freq, int n, int limit, uint8_t *lens) {
    memset(lens, 0, (size_t)n);
    int32_t leaves[MAX_MAIN];
    int m = 0;
    for (int i = 0; i < n; ++i) {
        if (freq[i]) leaves[m++] = i;
    }
    if (m < 2) {
        int a = m ? leaves[0] : 0, b = a == 0 ? 1 : 0;
        lens[a] = lens[b] = 1;
        return true;
    }
    for (int i = 1; i < m; ++i) {
        for (int j = i; j > 0; --j) {
            int32_t x = leaves[j - 1], y = leaves[j];
            if (freq[x] < freq[y] || (freq[x] == freq[y] && x < y)) break;
            leaves[j - 1] = y;
            leaves[j] = x;
        }
    }
    pm_item_t *lists[16] = { 0 };
    int32_t sizes[16] = { 0 };
    bool ok = true;
    for (int lev = 0; lev < limit && ok; ++lev) {
        int32_t npk = lev == 0 ? 0 : sizes[lev - 1] / 2;
        lists[lev] = rp_mem_alloc(alloc, (size_t)(m + npk), sizeof(pm_item_t));
        if (lists[lev] == NULL) {
            ok = false;
            break;
        }
        int32_t li = 0, pi = 0, o = 0;
        while (li < m || pi < npk) {
            uint64_t lw = li < m ? freq[leaves[li]] : UINT64_MAX;
            uint64_t pw = pi < npk ? lists[lev - 1][2 * pi].w + lists[lev - 1][2 * pi + 1].w : UINT64_MAX;
            if (lw <= pw) {
                lists[lev][o++] = (pm_item_t){ lw, leaves[li], 0, 0 };
                ++li;
            } else {
                lists[lev][o++] = (pm_item_t){ pw, -1, 2 * pi, 2 * pi + 1 };
                ++pi;
            }
        }
        sizes[lev] = o;
    }
    if (ok) {
        for (int32_t k = 0; k < 2 * m - 2; ++k) pm_count(lists, limit - 1, k, lens);
    }
    for (int lev = 0; lev < 16; ++lev) rp_mem_free(alloc, lists[lev]);
    return ok;
}

// Canonical codes: shorter codes first, and within a length in symbol order.
static void make_codes(const uint8_t *lens, int n, uint32_t *codes) {
    uint32_t count[18] = { 0 }, next[18] = { 0 };
    for (int i = 0; i < n; ++i) count[lens[i]]++;
    count[0] = 0;
    uint32_t code = 0;
    for (int b = 1; b <= 16; ++b) {
        code = (code + count[b - 1]) << 1;
        next[b] = code;
    }
    for (int i = 0; i < n; ++i) codes[i] = lens[i] ? next[lens[i]]++ : 0;
}

// ---- encoder ---------------------------------------------------------------------------------

struct rp_lzx_enc {
    proven_allocator_t alloc;
    uint32_t           wsize;
    int                slots, main_n;
    uint8_t           *buf;         // the last wsize bytes or more, then the frame being compressed
    size_t             cap, len;
    int32_t           *head, *prev; // hash chains over buf indexes (-1: none)
    uint8_t            main_prev[MAX_MAIN], len_prev[SECONDARY];
    bool               started;
    uint8_t           *out;
    size_t             out_cap, out_len;
    uint64_t           bitbuf;
    int                bits;
    bool               nomem;
    uint32_t          *tok;         // literal byte, or 0x80000000 | (length - 2) << 21 | slot << 24 ... see enc_frame
    size_t             ntok;
    uint64_t           done;        // bytes compressed so far in the folder
    uint32_t           r[3];        // the repeated offsets, as the decoder keeps them
};

static void put(rp_lzx_enc_t *e, uint32_t v, int n) {
    e->bitbuf = e->bitbuf << n | (v & ((1u << n) - 1u));
    e->bits += n;
    while (e->bits >= 16) {
        uint16_t w = (uint16_t)(e->bitbuf >> (e->bits - 16));
        e->bits -= 16;
        if (e->out_len + 2 > e->out_cap) {
            size_t cap = e->out_cap * 2;
            uint8_t *o = rp_mem_alloc(e->alloc, cap, 1);
            if (o == NULL) {
                e->nomem = true;
                return;
            }
            memcpy(o, e->out, e->out_len);
            rp_mem_free(e->alloc, e->out);
            e->out = o;
            e->out_cap = cap;
        }
        e->out[e->out_len++] = (uint8_t)w;
        e->out[e->out_len++] = (uint8_t)(w >> 8);
    }
}

void rp_lzx_enc_free(rp_lzx_enc_t *e) {
    if (e == NULL) return;
    rp_mem_free(e->alloc, e->buf);
    rp_mem_free(e->alloc, e->head);
    rp_mem_free(e->alloc, e->prev);
    rp_mem_free(e->alloc, e->out);
    rp_mem_free(e->alloc, e->tok);
    rp_mem_free(e->alloc, e);
}

proven_err_t rp_lzx_enc_new(proven_allocator_t alloc, unsigned wbits, rp_lzx_enc_t **out) {
    if (out == NULL || wbits < RP_LZX_MIN_WBITS || wbits > RP_LZX_MAX_WBITS) return PROVEN_ERR_INVALID_ARG;
    rp_lzx_enc_t *e = rp_mem_alloc(alloc, 1, sizeof *e);
    if (e == NULL) return PROVEN_ERR_NOMEM;
    memset(e, 0, sizeof *e);
    e->alloc = alloc;
    e->wsize = 1u << wbits;
    e->slots = slots_for[wbits - RP_LZX_MIN_WBITS];
    e->main_n = 256 + e->slots * 8;
    e->cap = 2 * (size_t)e->wsize + RP_LZX_FRAME;
    e->buf = rp_mem_alloc(alloc, e->cap, 1);
    e->head = rp_mem_alloc(alloc, (size_t)1 << HASH_BITS, sizeof *e->head);
    e->prev = rp_mem_alloc(alloc, e->cap, sizeof *e->prev);
    e->out_cap = 3 * RP_LZX_FRAME;
    e->out = rp_mem_alloc(alloc, e->out_cap, 1);
    e->tok = rp_mem_alloc(alloc, RP_LZX_FRAME, sizeof *e->tok);
    if (e->buf == NULL || e->head == NULL || e->prev == NULL || e->out == NULL || e->tok == NULL) {
        rp_lzx_enc_free(e);
        return PROVEN_ERR_NOMEM;
    }
    for (size_t i = 0; i < (size_t)1 << HASH_BITS; ++i) e->head[i] = -1;
    e->r[0] = e->r[1] = e->r[2] = 1;
    *out = e;
    return PROVEN_OK;
}

static uint32_t hash3(const uint8_t *p) {
    return (((uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2]) * 2654435761u) >> (32 - HASH_BITS);
}

// Bytes a[0..) and b[0..) have in common, up to lim; eight at a time.
static size_t common(const uint8_t *a, const uint8_t *b, size_t lim) {
    size_t l = 0;
    while (l + 8 <= lim) {
        uint64_t x, y;
        memcpy(&x, a + l, 8);
        memcpy(&y, b + l, 8);
        if (x != y) break;
        l += 8;
    }
    while (l < lim && a[l] == b[l]) ++l;
    return l;
}

static void insert(rp_lzx_enc_t *e, size_t p) {
    uint32_t h = hash3(e->buf + p);
    e->prev[p] = e->head[h];
    e->head[h] = (int32_t)p;
}

// The pretree-coded lengths of cur[a..b), as differences from prev[a..b).
static void write_lengths(rp_lzx_enc_t *e, const uint8_t *cur, const uint8_t *prev, int a, int b) {
    uint8_t sym[MAX_MAIN];
    uint8_t extra[MAX_MAIN];
    int ns = 0;
    for (int x = a; x < b;) {
        int run = 0;
        while (x + run < b && cur[x + run] == 0) ++run;
        if (run >= 20) {
            int take = run > 51 ? 51 : run;
            sym[ns] = 18;
            extra[ns++] = (uint8_t)(take - 20);
            x += take;
        } else if (run >= 4) {
            sym[ns] = 17;
            extra[ns++] = (uint8_t)(run - 4);
            x += run;
        } else {
            sym[ns] = (uint8_t)((prev[x] - cur[x] + 17) % 17);
            extra[ns++] = 0;
            ++x;
        }
    }
    uint32_t freq[PRETREE] = { 0 }, codes[PRETREE];
    uint8_t lens[PRETREE];
    for (int i = 0; i < ns; ++i) freq[sym[i]]++;
    if (!build_lengths(e->alloc, freq, PRETREE, 15, lens)) {
        e->nomem = true;
        return;
    }
    make_codes(lens, PRETREE, codes);
    for (int i = 0; i < PRETREE; ++i) put(e, lens[i], 4);
    for (int i = 0; i < ns; ++i) {
        put(e, codes[sym[i]], lens[sym[i]]);
        if (sym[i] == 18) put(e, extra[i], 5);
        else if (sym[i] == 17) put(e, extra[i], 4);
    }
}

// Rough bits a match saves over literals (about 6 bits a literal, 9 for the main symbol, plus the
// offset's extra bits): picks between candidates and turns down far short matches.
static int score(size_t len, uint32_t extra) { return (int)len * 6 - 9 - (int)extra; }

static int slot_of(uint32_t formatted, int slots) {
    int lo = 3, hi = slots - 1;         // the last slot whose base is <= formatted
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (base_tab[mid] <= formatted) lo = mid;
        else hi = mid - 1;
    }
    return lo;
}

proven_err_t rp_lzx_enc_frame(rp_lzx_enc_t *e, const uint8_t *in, size_t n, const uint8_t **out, size_t *out_len) {
    if (e == NULL || in == NULL || n == 0 || n > RP_LZX_FRAME || out == NULL || out_len == NULL) return PROVEN_ERR_INVALID_ARG;
    // Keep the last window's bytes; drop older ones in one step once the buffer is full.
    if (e->len + n > e->cap) {
        size_t drop = e->len - e->wsize;
        memmove(e->buf, e->buf + drop, e->wsize);
        for (size_t i = 0; i < (size_t)1 << HASH_BITS; ++i) e->head[i] = e->head[i] >= (int32_t)drop ? e->head[i] - (int32_t)drop : -1;
        for (size_t i = 0; i < e->wsize; ++i) e->prev[i] = e->prev[i + drop] >= (int32_t)drop ? e->prev[i + drop] - (int32_t)drop : -1;
        e->len = e->wsize;
    }
    size_t fs = e->len, fe = fs + n;
    memcpy(e->buf + fs, in, n);
    const size_t max_dist = e->wsize - 3;
    const uint64_t done = e->done;      // bytes of the folder before this frame
    uint32_t r[3] = { e->r[0], e->r[1], e->r[2] };

    // Matches of 3 or more bytes, none reaching past the frame: a repeated offset when it is about
    // as long as the best new one (it costs no offset bits), and one step of lazy evaluation.
    // A token is a literal byte, or 0x80000000 | (length - 2) << 21 | offset code: 0-2 a repeated
    // offset, else the distance + 2 (the formatted offset).
    e->ntok = 0;
    size_t pend_len = 0, ins = fs;      // ins: the next position to put in the hash chains
    uint32_t pend_code = 0;
    for (size_t p = fs; p < fe;) {
        size_t lim = fe - p < MAX_MATCH ? fe - p : MAX_MATCH, best = 0, best_d = 0, rep_len = 0;
        int rep = -1;
        for (int k = 0; k < 3; ++k) {
            if (r[k] > done + (p - fs) || r[k] > max_dist || (size_t)r[k] > p) continue;
            size_t l = common(e->buf + p - r[k], e->buf + p, lim);
            if (l > rep_len) {
                rep_len = l;
                rep = k;
            }
        }
        if (fe - p >= 3) {
            int32_t c = e->head[hash3(e->buf + p)];
            int best_s = 0;
            for (int depth = CHAIN; c >= 0 && depth > 0; --depth, c = e->prev[c]) {
                size_t d = p - (size_t)c;
                if (d > max_dist) break;
                // Candidates come nearest first: a farther one must be longer to score higher.
                if (best && e->buf[c + best] != e->buf[p + best]) continue;
                size_t l = common(e->buf + c, e->buf + p, lim);
                if (l < 3 || l <= best) continue;
                int sc = score(l, extra_bits(slot_of((uint32_t)d + 2, e->slots)));
                if (sc > best_s) {
                    best = l;
                    best_d = d;
                    best_s = sc;
                    if (l >= NICE || l == lim) break;
                }
            }
            if (p >= ins) {
                insert(e, p);
                ins = p + 1;
            }
        }
        size_t len = 0;
        uint32_t code = 0;
        if (rep_len >= 3 && (best < 3 || score(rep_len, 0) >= score(best, extra_bits(slot_of((uint32_t)best_d + 2, e->slots))))) {
            len = rep_len;
            code = (uint32_t)rep;
        } else if (best >= 3) {
            len = best;
            code = (uint32_t)best_d + 2;
        }
        // Lazy: a match found one byte earlier waits for this one; the longer wins.
        if (pend_len) {
            if (len > pend_len + 1) {
                e->tok[e->ntok++] = e->buf[p - 1];      // the earlier byte goes as a literal
            } else {
                len = pend_len;
                code = pend_code;
                p -= 1;
                goto emit;
            }
            pend_len = 0;
        }
        if (len >= 3 && len < 32 && p + 1 < fe) {
            pend_len = len;
            pend_code = code;
            ++p;
            continue;
        }
        if (len < 3) {
            e->tok[e->ntok++] = e->buf[p];
            ++p;
            continue;
        }
    emit:
        pend_len = 0;
        e->tok[e->ntok++] = 0x80000000u | (uint32_t)(len - MIN_MATCH) << 21 | code;
        if (code == 1) {
            uint32_t t = r[0];
            r[0] = r[1];
            r[1] = t;
        } else if (code == 2) {
            uint32_t t = r[0];
            r[0] = r[2];
            r[2] = t;
        } else if (code > 2) {
            r[2] = r[1];
            r[1] = r[0];
            r[0] = code - 2;
        }
        for (size_t q = ins > p + 1 ? ins : p + 1; q < p + len && q + 3 <= fe; ++q) insert(e, q);
        if (ins < p + len) ins = p + len;
        p += len;
    }
    if (pend_len) e->tok[e->ntok++] = e->buf[fe - 1];

    // One verbatim block for the frame; an uncompressed block instead when that is not smaller.
    uint32_t mf[MAX_MAIN] = { 0 }, lf[SECONDARY] = { 0 }, mc[MAX_MAIN], lc[SECONDARY];
    uint8_t ml[MAX_MAIN], ll[SECONDARY];
    for (size_t i = 0; i < e->ntok; ++i) {
        uint32_t t = e->tok[i];
        if (!(t & 0x80000000u)) {
            mf[t]++;
            continue;
        }
        uint32_t len = (t >> 21 & 0xFF) + MIN_MATCH, code = t & 0x1FFFFF;
        int slot = code < 3 ? (int)code : slot_of(code, e->slots);
        uint32_t lh = len - MIN_MATCH < PRIMARY ? len - MIN_MATCH : PRIMARY;
        mf[256 + slot * 8 + (int)lh]++;
        if (lh == PRIMARY) lf[len - MIN_MATCH - PRIMARY]++;
    }
    if (!build_lengths(e->alloc, mf, e->main_n, 16, ml) || !build_lengths(e->alloc, lf, SECONDARY, 16, ll)) return PROVEN_ERR_NOMEM;
    make_codes(ml, e->main_n, mc);
    make_codes(ll, SECONDARY, lc);
    e->out_len = 0;
    if (!e->started) {
        put(e, 0, 1);       // no E8 translation
        e->started = true;
    }
    size_t mark_len = e->out_len;
    int mark_bits = e->bits;
    uint64_t mark_buf = e->bitbuf;
    put(e, TYPE_VERBATIM, 3);
    put(e, (uint32_t)(n >> 8), 16);
    put(e, (uint32_t)(n & 0xFF), 8);
    write_lengths(e, ml, e->main_prev, 0, 256);
    write_lengths(e, ml, e->main_prev, 256, e->main_n);
    write_lengths(e, ll, e->len_prev, 0, SECONDARY);
    for (size_t i = 0; i < e->ntok; ++i) {
        uint32_t t = e->tok[i];
        if (!(t & 0x80000000u)) {
            put(e, mc[t], ml[t]);
            continue;
        }
        uint32_t len = (t >> 21 & 0xFF) + MIN_MATCH, code = t & 0x1FFFFF;
        int slot = code < 3 ? (int)code : slot_of(code, e->slots);
        uint32_t lh = len - MIN_MATCH < PRIMARY ? len - MIN_MATCH : PRIMARY;
        int sym = 256 + slot * 8 + (int)lh;
        put(e, mc[sym], ml[sym]);
        if (lh == PRIMARY) put(e, lc[len - MIN_MATCH - PRIMARY], ll[len - MIN_MATCH - PRIMARY]);
        if (slot >= 3 && extra_bits(slot)) put(e, code - slot_base(slot), extra_bits(slot));
    }
    if (e->bits) put(e, 0, 16 - e->bits);       // the frame ends on a 16-bit boundary
    if (e->out_len - mark_len <= n + 16) {
        memcpy(e->main_prev, ml, (size_t)e->main_n);
        memcpy(e->len_prev, ll, SECONDARY);
        memcpy(e->r, r, sizeof r);
    } else {
        // Uncompressed: header, padding to 16 bits (a whole word when already aligned), the three
        // repeated offsets, the bytes, one more byte when their count is odd. Trees and repeated
        // offsets stay as they were.
        e->out_len = mark_len;
        e->bits = mark_bits;
        e->bitbuf = mark_buf;
        put(e, TYPE_UNCOMPRESSED, 3);
        put(e, (uint32_t)(n >> 8), 16);
        put(e, (uint32_t)(n & 0xFF), 8);
        put(e, 0, e->bits ? 16 - e->bits : 16);
        // put() writes a word low byte first: bytes a, b go as the word b << 8 | a.
        for (int k = 0; k < 3; ++k) {
            put(e, e->r[k] & 0xFFFF, 16);
            put(e, e->r[k] >> 16, 16);
        }
        for (size_t i = 0; i + 1 < n; i += 2) put(e, (uint32_t)in[i + 1] << 8 | in[i], 16);
        if (n & 1) put(e, in[n - 1], 16);
    }
    if (e->nomem) return PROVEN_ERR_NOMEM;
    e->len = fe;
    e->done += n;
    *out = e->out;
    *out_len = e->out_len;
    return PROVEN_OK;
}

// ---- decoder ---------------------------------------------------------------------------------

typedef struct {
    uint16_t count[17];
    uint16_t sym[MAX_MAIN];
    bool     empty;
} huff_t;

struct rp_lzx_dec {
    proven_allocator_t alloc;
    uint32_t           wsize;
    int                slots, main_n;
    uint8_t           *win;
    uint64_t           pos, frame_start;
    uint32_t           r0, r1, r2;
    uint8_t            main_len[MAX_MAIN], len_len[SECONDARY], aligned_len[ALIGNED];
    huff_t             main, length, aligned;
    int                block_type;
    uint32_t           block_remaining, block_length;
    bool               header_read, intel_started, pad_pending;
    uint32_t           intel_size, frames;
    int32_t            intel_pos;
    const uint8_t     *in;
    size_t             in_len, in_pos;
    uint64_t           bitbuf;
    int                bits;
};

void rp_lzx_dec_free(rp_lzx_dec_t *d) {
    if (d == NULL) return;
    rp_mem_free(d->alloc, d->win);
    rp_mem_free(d->alloc, d);
}

proven_err_t rp_lzx_dec_new(proven_allocator_t alloc, unsigned wbits, rp_lzx_dec_t **out) {
    if (out == NULL || wbits < RP_LZX_MIN_WBITS || wbits > RP_LZX_MAX_WBITS) return PROVEN_ERR_INVALID_ARG;
    rp_lzx_dec_t *d = rp_mem_alloc(alloc, 1, sizeof *d);
    if (d == NULL) return PROVEN_ERR_NOMEM;
    memset(d, 0, sizeof *d);
    d->alloc = alloc;
    d->wsize = 1u << wbits;
    d->slots = slots_for[wbits - RP_LZX_MIN_WBITS];
    d->main_n = 256 + d->slots * 8;
    d->win = rp_mem_alloc(alloc, d->wsize, 1);
    if (d->win == NULL) {
        rp_mem_free(alloc, d);
        return PROVEN_ERR_NOMEM;
    }
    memset(d->win, 0, d->wsize);
    d->r0 = d->r1 = d->r2 = 1;
    *out = d;
    return PROVEN_OK;
}

static bool need(rp_lzx_dec_t *d, int n) {
    while (d->bits < n) {
        if (d->in_len - d->in_pos < 2) return false;
        d->bitbuf = d->bitbuf << 16 | (uint32_t)(d->in[d->in_pos] | d->in[d->in_pos + 1] << 8);
        d->in_pos += 2;
        d->bits += 16;
    }
    return true;
}

static bool getbits(rp_lzx_dec_t *d, int n, uint32_t *v) {
    if (n == 0) {
        *v = 0;
        return true;
    }
    if (!need(d, n)) return false;
    d->bits -= n;
    *v = (uint32_t)(d->bitbuf >> d->bits) & ((1u << n) - 1u);
    return true;
}

// Builds a canonical decoder; an over-subscribed or incomplete code is refused, an empty one is
// allowed only where `empty_ok` (a length tree no match uses).
static bool huff_build(huff_t *h, const uint8_t *lens, int n, bool empty_ok) {
    memset(h->count, 0, sizeof h->count);
    for (int i = 0; i < n; ++i) h->count[lens[i]]++;
    h->empty = h->count[0] == n;
    if (h->empty) return empty_ok;
    int left = 1;
    for (int len = 1; len <= 16; ++len) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return false;
    }
    if (left > 0) return false;
    uint16_t offs[17];
    offs[1] = 0;
    for (int len = 1; len < 16; ++len) offs[len + 1] = (uint16_t)(offs[len] + h->count[len]);
    for (int i = 0; i < n; ++i) {
        if (lens[i]) h->sym[offs[lens[i]]++] = (uint16_t)i;
    }
    return true;
}

static int decode(rp_lzx_dec_t *d, const huff_t *h) {
    if (h->empty) return -1;
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 16; ++len) {
        uint32_t bit;
        if (!getbits(d, 1, &bit)) return -1;
        code |= (int)bit;
        int count = h->count[len];
        if (code - first < count) return h->sym[index + code - first];
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return -1;
}

static bool read_lengths(rp_lzx_dec_t *d, uint8_t *lens, int first, int last) {
    uint8_t pl[PRETREE];
    huff_t pre;
    for (int i = 0; i < PRETREE; ++i) {
        uint32_t v;
        if (!getbits(d, 4, &v)) return false;
        pl[i] = (uint8_t)v;
    }
    if (!huff_build(&pre, pl, PRETREE, false)) return false;
    for (int x = first; x < last;) {
        int z = decode(d, &pre);
        uint32_t y;
        if (z < 0) return false;
        if (z == 17 || z == 18) {
            if (!getbits(d, z == 17 ? 4 : 5, &y)) return false;
            y += z == 17 ? 4 : 20;
            if (y > (uint32_t)(last - x)) return false;
            while (y--) lens[x++] = 0;
        } else if (z == 19) {
            if (!getbits(d, 1, &y)) return false;
            y += 4;
            if (y > (uint32_t)(last - x)) return false;
            z = decode(d, &pre);
            if (z < 0 || z > 16) return false;
            int v = lens[x] - z;
            if (v < 0) v += 17;
            while (y--) lens[x++] = (uint8_t)v;
        } else {
            int v = lens[x] - z;
            if (v < 0) v += 17;
            lens[x++] = (uint8_t)v;
        }
    }
    return true;
}

static proven_err_t block_header(rp_lzx_dec_t *d) {
    uint32_t type, hi, lo;
    if (d->block_type == TYPE_UNCOMPRESSED) {
        if (d->pad_pending) {               // the padding byte of an odd block the last frame ended with
            if (d->in_pos >= d->in_len) return PROVEN_ERR_INVALID_FORMAT;
            d->in_pos++;
            d->pad_pending = false;
        }
        d->bits = 0;
    }
    if (!getbits(d, 3, &type) || !getbits(d, 16, &hi) || !getbits(d, 8, &lo)) return PROVEN_ERR_INVALID_FORMAT;
    d->block_type = (int)type;
    d->block_remaining = d->block_length = hi << 8 | lo;
    switch (type) {
    case TYPE_ALIGNED:
        for (int i = 0; i < ALIGNED; ++i) {
            uint32_t v;
            if (!getbits(d, 3, &v)) return PROVEN_ERR_INVALID_FORMAT;
            d->aligned_len[i] = (uint8_t)v;
        }
        if (!huff_build(&d->aligned, d->aligned_len, ALIGNED, false)) return PROVEN_ERR_INVALID_FORMAT;
        [[fallthrough]];
    case TYPE_VERBATIM:
        if (!read_lengths(d, d->main_len, 0, 256) || !read_lengths(d, d->main_len, 256, d->main_n) ||
            !huff_build(&d->main, d->main_len, d->main_n, false)) {
            return PROVEN_ERR_INVALID_FORMAT;
        }
        if (d->main_len[0xE8]) d->intel_started = true;
        if (!read_lengths(d, d->len_len, 0, SECONDARY) || !huff_build(&d->length, d->len_len, SECONDARY, true)) {
            return PROVEN_ERR_INVALID_FORMAT;
        }
        return PROVEN_OK;
    case TYPE_UNCOMPRESSED:
        d->intel_started = true;
        // Padding to a 16-bit boundary: the rest of the current word, or a whole word when aligned.
        if (d->bits == 0) {
            if (d->in_len - d->in_pos < 2) return PROVEN_ERR_INVALID_FORMAT;
            d->in_pos += 2;
        }
        d->bits = 0;
        if (d->in_len - d->in_pos < 12) return PROVEN_ERR_INVALID_FORMAT;
        uint32_t r[3];
        for (int i = 0; i < 3; ++i) {
            const uint8_t *p = d->in + d->in_pos + 4 * (size_t)i;
            r[i] = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
        }
        d->in_pos += 12;
        d->r0 = r[0];
        d->r1 = r[1];
        d->r2 = r[2];
        return PROVEN_OK;
    default:
        return PROVEN_ERR_INVALID_FORMAT;
    }
}

proven_err_t rp_lzx_dec_frame(rp_lzx_dec_t *d, const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len) {
    if (d == NULL || (in == NULL && in_len) || out == NULL || out_len == 0 || out_len > RP_LZX_FRAME) return PROVEN_ERR_INVALID_ARG;
    d->in = in;
    d->in_len = in_len;
    d->in_pos = 0;
    d->bits = 0;
    const uint32_t mask = d->wsize - 1;
    uint64_t end = d->frame_start + out_len;
    if (!d->header_read) {
        uint32_t e8, hi = 0, lo = 0;
        if (!getbits(d, 1, &e8) || (e8 && (!getbits(d, 16, &hi) || !getbits(d, 16, &lo)))) return PROVEN_ERR_INVALID_FORMAT;
        d->intel_size = hi << 16 | lo;
        d->header_read = true;
    }
    while (d->pos < end) {
        if (d->block_remaining == 0) {
            proven_err_t err = block_header(d);
            if (err != PROVEN_OK) return err;
            continue;
        }
        if (d->block_type == TYPE_UNCOMPRESSED) {
            if (d->in_pos >= d->in_len) return PROVEN_ERR_INVALID_FORMAT;
            d->win[d->pos++ & mask] = d->in[d->in_pos++];
            if (--d->block_remaining == 0 && (d->block_length & 1)) {
                // An odd uncompressed block is padded to an even size: take the byte now if this
                // frame's data holds it, else at the next block header.
                if (d->in_pos < d->in_len) d->in_pos++;
                else d->pad_pending = true;
            }
            continue;
        }
        int sym = decode(d, &d->main);
        if (sym < 0) return PROVEN_ERR_INVALID_FORMAT;
        if (sym < 256) {
            d->win[d->pos++ & mask] = (uint8_t)sym;
            d->block_remaining--;
            continue;
        }
        sym -= 256;
        uint32_t len = (uint32_t)(sym & 7), off;
        if (len == PRIMARY) {
            int l = decode(d, &d->length);
            if (l < 0) return PROVEN_ERR_INVALID_FORMAT;
            len += (uint32_t)l;
        }
        len += MIN_MATCH;
        int slot = sym >> 3;
        if (slot == 0) {
            off = d->r0;
        } else if (slot == 1) {
            off = d->r1;
            d->r1 = d->r0;
            d->r0 = off;
        } else if (slot == 2) {
            off = d->r2;
            d->r2 = d->r0;
            d->r0 = off;
        } else {
            int extra = extra_bits(slot);
            uint32_t v = 0, a = 0;
            off = slot_base(slot) - 2;
            if (d->block_type == TYPE_ALIGNED && extra >= 3) {
                if (!getbits(d, extra - 3, &v)) return PROVEN_ERR_INVALID_FORMAT;
                int s = decode(d, &d->aligned);
                if (s < 0) return PROVEN_ERR_INVALID_FORMAT;
                a = (uint32_t)s;
                off += (v << 3) + a;
            } else {
                if (!getbits(d, extra, &v)) return PROVEN_ERR_INVALID_FORMAT;
                off += v;
            }
            d->r2 = d->r1;
            d->r1 = d->r0;
            d->r0 = off;
        }
        if (len > d->block_remaining || off == 0 || off > d->pos || off > d->wsize) return PROVEN_ERR_INVALID_FORMAT;
        for (uint32_t k = 0; k < len; ++k, ++d->pos) d->win[d->pos & mask] = d->win[(d->pos - off) & mask];
        d->block_remaining -= len;
    }
    for (size_t i = 0; i < out_len; ++i) out[i] = d->win[(d->frame_start + i) & mask];
    // E8 translation: call targets written as absolute offsets go back to relative ones.
    if (d->intel_started && d->intel_size && d->frames < 32768 && out_len > 10) {
        int32_t cur = d->intel_pos, size = (int32_t)d->intel_size;
        for (size_t i = 0; i < out_len - 10;) {
            if (out[i++] != 0xE8) {
                cur++;
                continue;
            }
            int32_t abs_off = (int32_t)((uint32_t)out[i] | (uint32_t)out[i + 1] << 8 | (uint32_t)out[i + 2] << 16 | (uint32_t)out[i + 3] << 24);
            if (abs_off >= -cur && abs_off < size) {
                int32_t rel = abs_off >= 0 ? abs_off - cur : abs_off + size;
                out[i] = (uint8_t)rel;
                out[i + 1] = (uint8_t)(rel >> 8);
                out[i + 2] = (uint8_t)(rel >> 16);
                out[i + 3] = (uint8_t)((uint32_t)rel >> 24);
            }
            i += 4;
            cur += 5;
        }
    }
    if (d->intel_size) d->intel_pos += (int32_t)out_len;
    d->frames++;
    d->frame_start = end;
    d->bits = 0;            // the frame ends on a 16-bit boundary
    return PROVEN_OK;
}
