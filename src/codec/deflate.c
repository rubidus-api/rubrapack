// src/codec/deflate.c - raw deflate encoder and decoder from RFC 1951 (include/rubrapack/deflate.h).
//
// Encoder: LZ77 with 3-byte hash chains (lazy matching from level 4), blocks of up to 32768
// symbols, each written in whichever of fixed Huffman / dynamic Huffman / stored is shortest.
// Dynamic code lengths come from package-merge, so they are optimal under the 15-bit (7-bit for
// the code-length alphabet) limit, and every code written is complete.

#include "rubrapack/buf.h"
#include "rubrapack/deflate.h"
#include "rubrapack/mem.h"

#include <string.h>

// ---- RFC 1951 3.2.5 tables -------------------------------------------------------------------

static const uint16_t len_base[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                       35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const uint8_t len_extra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const uint16_t dist_base[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385,
                                        513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const uint8_t dist_extra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
                                        6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
static const uint8_t clen_order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };

enum { WINDOW = 32768, MIN_MATCH = 3, MAX_MATCH = 258, BLOCK_SYMBOLS = 32768, LITLEN = 288, DISTS = 30 };

static int len_code(unsigned len) {         // index 0..28 for a match length 3..258
    int i = 28;
    while (len_base[i] > len) --i;
    return i;
}

static int dist_code(unsigned d) {          // index 0..29 for a distance 1..32768
    int i = 29;
    while (dist_base[i] > d) --i;
    return i;
}

// ---- bit writer ------------------------------------------------------------------------------

typedef struct {
    rp_buf_t b;
    uint64_t acc;
    unsigned n;
} bits_t;

static void put_bits(bits_t *w, uint32_t v, unsigned n) {
    w->acc |= (uint64_t)v << w->n;
    w->n += n;
    while (w->n >= 8) {
        rp_buf_byte(&w->b, (uint8_t)w->acc);
        w->acc >>= 8;
        w->n -= 8;
    }
}

static void put_code(bits_t *w, uint32_t code, unsigned len) {     // Huffman codes go MSB first
    uint32_t r = 0;
    for (unsigned k = 0; k < len; ++k) r |= ((code >> k) & 1u) << (len - 1 - k);
    put_bits(w, r, len);
}

static void align(bits_t *w) {
    if (w->n) put_bits(w, 0, 8 - w->n);
}

// ---- Huffman code lengths (package-merge) ----------------------------------------------------

typedef struct {
    uint64_t w;
    int32_t  leaf;          // symbol, or -1 for a package of items a and b of the level below
    int32_t  a, b;
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

// Code lengths <= limit for freq[0..n); symbols with frequency 0 get length 0. At least two
// symbols always get a length, so the code is complete (a lone symbol is paired with a dummy).
static bool build_lengths(proven_allocator_t alloc, const uint32_t *freq, int n, int limit, uint8_t *lens) {
    memset(lens, 0, (size_t)n);
    int32_t leaves[LITLEN];
    int m = 0;
    for (int i = 0; i < n; ++i) {
        if (freq[i]) leaves[m++] = i;
    }
    if (m == 0) {
        lens[0] = lens[1] = 1;
        return true;
    }
    if (m == 1) {
        lens[leaves[0]] = 1;
        lens[leaves[0] == 0 ? 1 : 0] = 1;
        return true;
    }
    // Sort leaves by frequency, then symbol (stable, deterministic).
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

// Canonical codes from lengths (RFC 1951 3.2.2).
static void make_codes(const uint8_t *lens, int n, uint16_t *codes) {
    uint16_t count[16] = { 0 }, next[16] = { 0 };
    for (int i = 0; i < n; ++i) count[lens[i]]++;
    count[0] = 0;
    uint16_t code = 0;
    for (int b = 1; b < 16; ++b) {
        code = (uint16_t)((code + count[b - 1]) << 1);
        next[b] = code;
    }
    for (int i = 0; i < n; ++i) codes[i] = lens[i] ? next[lens[i]]++ : 0;
}

// ---- LZ77 ------------------------------------------------------------------------------------

typedef struct {
    uint16_t lit;           // literal byte, or 256 + length for a match
    uint16_t dist;          // 0 for a literal
} sym_t;

static uint32_t hash3(const uint8_t *p) { return ((uint32_t)p[0] << 10 ^ (uint32_t)p[1] << 5 ^ p[2]) & 0x7FFF; }

// ---- block writing ---------------------------------------------------------------------------

typedef struct {
    uint8_t  litlen[LITLEN], dist[DISTS];
    uint16_t lcode[LITLEN], dcode[DISTS];
    // dynamic header
    uint8_t  rle[LITLEN + DISTS];
    uint8_t  rle_extra[LITLEN + DISTS];
    int      nrle;
    uint8_t  clen[19];
    uint16_t ccode[19];
    int      hlit, hdist, hclen;
} tree_t;

static uint64_t data_bits(const sym_t *s, size_t ns, const uint8_t *ll, const uint8_t *dl) {
    uint64_t bits = ll[256];
    for (size_t i = 0; i < ns; ++i) {
        if (s[i].dist == 0) {
            bits += ll[s[i].lit];
        } else {
            int lc = len_code(s[i].lit - 256u), dc = dist_code(s[i].dist);
            bits += ll[257 + lc] + len_extra[lc] + dl[dc] + dist_extra[dc];
        }
    }
    return bits;
}

static void fixed_tree(tree_t *t) {
    for (int i = 0; i < LITLEN; ++i) t->litlen[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
    for (int i = 0; i < DISTS; ++i) t->dist[i] = 5;
    make_codes(t->litlen, LITLEN, t->lcode);
    make_codes(t->dist, DISTS, t->dcode);
}

// Dynamic tree for a block; returns the header size in bits (or 0 on allocation failure).
static uint64_t dynamic_tree(proven_allocator_t alloc, tree_t *t, const sym_t *s, size_t ns) {
    uint32_t lf[LITLEN] = { 0 }, df[DISTS] = { 0 };
    lf[256] = 1;
    for (size_t i = 0; i < ns; ++i) {
        if (s[i].dist == 0) {
            lf[s[i].lit]++;
        } else {
            lf[257 + len_code(s[i].lit - 256u)]++;
            df[dist_code(s[i].dist)]++;
        }
    }
    if (!build_lengths(alloc, lf, 286, 15, t->litlen) || !build_lengths(alloc, df, DISTS, 15, t->dist)) return 0;
    t->litlen[286] = t->litlen[287] = 0;
    make_codes(t->litlen, LITLEN, t->lcode);
    make_codes(t->dist, DISTS, t->dcode);
    t->hlit = 286;
    while (t->hlit > 257 && t->litlen[t->hlit - 1] == 0) --t->hlit;
    t->hdist = DISTS;
    while (t->hdist > 1 && t->dist[t->hdist - 1] == 0) --t->hdist;

    // Run-length code the lengths (RFC 1951 3.2.7).
    uint8_t all[LITLEN + DISTS];
    int na = 0;
    for (int i = 0; i < t->hlit; ++i) all[na++] = t->litlen[i];
    for (int i = 0; i < t->hdist; ++i) all[na++] = t->dist[i];
    t->nrle = 0;
    uint32_t cf[19] = { 0 };
    for (int i = 0; i < na;) {
        int run = 1;
        while (i + run < na && all[i + run] == all[i]) ++run;
        if (all[i] == 0 && run >= 3) {
            int r = run > 138 ? 138 : run;
            t->rle[t->nrle] = r >= 11 ? 18 : 17;
            t->rle_extra[t->nrle++] = (uint8_t)(r >= 11 ? r - 11 : r - 3);
            cf[r >= 11 ? 18 : 17]++;
            i += r;
        } else if (all[i] != 0 && run >= 4) {
            t->rle[t->nrle] = all[i];
            t->rle_extra[t->nrle++] = 0;
            cf[all[i]]++;
            int r = run - 1 > 6 ? 6 : run - 1;
            t->rle[t->nrle] = 16;
            t->rle_extra[t->nrle++] = (uint8_t)(r - 3);
            cf[16]++;
            i += 1 + r;
        } else {
            t->rle[t->nrle] = all[i];
            t->rle_extra[t->nrle++] = 0;
            cf[all[i]]++;
            ++i;
        }
    }
    if (!build_lengths(alloc, cf, 19, 7, t->clen)) return 0;
    make_codes(t->clen, 19, t->ccode);
    t->hclen = 19;
    while (t->hclen > 4 && t->clen[clen_order[t->hclen - 1]] == 0) --t->hclen;
    uint64_t bits = 5 + 5 + 4 + 3u * (uint64_t)t->hclen;
    for (int i = 0; i < t->nrle; ++i) {
        uint8_t c = t->rle[i];
        bits += t->clen[c] + (c == 16 ? 2 : c == 17 ? 3 : c == 18 ? 7 : 0);
    }
    return bits;
}

static void write_symbols(bits_t *w, const tree_t *t, const sym_t *s, size_t ns) {
    for (size_t i = 0; i < ns; ++i) {
        if (s[i].dist == 0) {
            put_code(w, t->lcode[s[i].lit], t->litlen[s[i].lit]);
        } else {
            unsigned len = s[i].lit - 256u;
            int lc = len_code(len), dc = dist_code(s[i].dist);
            put_code(w, t->lcode[257 + lc], t->litlen[257 + lc]);
            if (len_extra[lc]) put_bits(w, len - len_base[lc], len_extra[lc]);
            put_code(w, t->dcode[dc], t->dist[dc]);
            if (dist_extra[dc]) put_bits(w, s[i].dist - dist_base[dc], dist_extra[dc]);
        }
    }
    put_code(w, t->lcode[256], t->litlen[256]);
}

static void write_stored(bits_t *w, const uint8_t *p, size_t n, bool final) {
    do {
        size_t take = n > 65535 ? 65535 : n;
        bool last = final && take == n;
        put_bits(w, last ? 1 : 0, 1);
        put_bits(w, 0, 2);
        align(w);
        put_bits(w, (uint32_t)take, 16);
        put_bits(w, (uint32_t)(~take & 0xFFFF), 16);
        rp_buf_put(&w->b, p, take);
        p += take;
        n -= take;
    } while (n > 0);
}

// A whole stream (last block final), or with `segment` a part of one: every block non-final, then
// an empty stored block, which ends the part on a byte boundary (00 00 FF FF, a "full flush").
static proven_err_t run(proven_allocator_t alloc, const uint8_t *in, size_t n, int level, bool segment, uint8_t **out, size_t *out_len) {
    if ((in == NULL && n != 0) || out == NULL || out_len == NULL || level < 0 || level > 9) return PROVEN_ERR_INVALID_ARG;
    bits_t w = { .b = rp_buf_new(alloc, n + n / 8 + 1024) };
    if (level == 0 || n == 0) {
        if (n || !segment) write_stored(&w, in, n, !segment);
        if (segment) write_stored(&w, NULL, 0, false);
        align(&w);
        return rp_buf_take(&w.b, out, out_len);
    }
    static const int chains[10] = { 0, 4, 8, 16, 32, 64, 128, 256, 1024, 4096 };
    int max_chain = chains[level];
    bool lazy = level >= 4;

    int32_t *head = rp_mem_alloc(alloc, 32768, sizeof *head);
    int32_t *prev = rp_mem_alloc(alloc, WINDOW, sizeof *prev);
    sym_t *syms = rp_mem_alloc(alloc, BLOCK_SYMBOLS, sizeof *syms);
    tree_t *dyn = rp_mem_alloc(alloc, 1, sizeof *dyn);
    tree_t *fix = rp_mem_alloc(alloc, 1, sizeof *fix);
    if (head == NULL || prev == NULL || syms == NULL || dyn == NULL || fix == NULL) {
        rp_mem_free(alloc, head);
        rp_mem_free(alloc, prev);
        rp_mem_free(alloc, syms);
        rp_mem_free(alloc, dyn);
        rp_mem_free(alloc, fix);
        rp_buf_free(&w.b);
        return PROVEN_ERR_NOMEM;
    }
    for (int i = 0; i < 32768; ++i) head[i] = -1;
    fixed_tree(fix);
    proven_err_t err = PROVEN_OK;

    size_t pos = 0;
    while (pos < n && err == PROVEN_OK) {
        size_t block_start = pos, ns = 0;
        while (pos < n && ns < BLOCK_SYMBOLS) {
            // Longest match at pos (and, when lazy, at pos + 1).
            size_t best_len = 0, best_dist = 0;
            for (int pass = 0; pass < (lazy ? 2 : 1); ++pass) {
                size_t at = pos + (size_t)pass;
                size_t found_len = 0, found_dist = 0;
                if (at + MIN_MATCH <= n) {
                    int32_t cand = head[hash3(in + at)];
                    int chain = max_chain;
                    size_t limit = n - at < MAX_MATCH ? n - at : MAX_MATCH;
                    while (cand >= 0 && chain-- > 0 && at - (size_t)cand <= WINDOW) {
                        const uint8_t *a = in + cand, *b = in + at;
                        size_t l = 0;
                        while (l < limit && a[l] == b[l]) ++l;
                        if (l > found_len) {
                            found_len = l;
                            found_dist = at - (size_t)cand;
                            if (l == limit) break;
                        }
                        cand = prev[cand % WINDOW];
                    }
                }
                if (found_len < MIN_MATCH) found_len = 0;
                if (pass == 0) {
                    best_len = found_len;
                    best_dist = found_dist;
                    if (best_len == 0) break;
                    // insert pos before looking at pos + 1
                    uint32_t h = hash3(in + pos);
                    prev[pos % WINDOW] = head[h];
                    head[h] = (int32_t)pos;
                } else if (found_len > best_len) {
                    best_len = 0;       // a better match starts one byte later: emit a literal
                }
            }
            if (best_len == 0) {
                if (pos + MIN_MATCH <= n) {
                    uint32_t h = hash3(in + pos);
                    if (head[h] != (int32_t)pos) {
                        prev[pos % WINDOW] = head[h];
                        head[h] = (int32_t)pos;
                    }
                }
                syms[ns++] = (sym_t){ in[pos], 0 };
                ++pos;
            } else {
                syms[ns++] = (sym_t){ (uint16_t)(256 + best_len), (uint16_t)best_dist };
                // pos itself is already in the chains; insert the rest of the match.
                for (size_t k = 1; k < best_len; ++k) {
                    size_t q = pos + k;
                    if (q + MIN_MATCH <= n) {
                        uint32_t h = hash3(in + q);
                        prev[q % WINDOW] = head[h];
                        head[h] = (int32_t)q;
                    }
                }
                pos += best_len;
            }
        }
        bool final = pos >= n && !segment;
        uint64_t dyn_header = dynamic_tree(alloc, dyn, syms, ns);
        if (dyn_header == 0) {
            err = PROVEN_ERR_NOMEM;
            break;
        }
        uint64_t dyn_bits = 3 + dyn_header + data_bits(syms, ns, dyn->litlen, dyn->dist);
        uint64_t fix_bits = 3 + data_bits(syms, ns, fix->litlen, fix->dist);
        size_t raw = pos - block_start;
        uint64_t stored_bits = (uint64_t)raw * 8 + ((raw + 65534) / 65535) * (3 + 7 + 32);
        if (stored_bits <= dyn_bits && stored_bits <= fix_bits) {
            write_stored(&w, in + block_start, raw, final);
        } else if (fix_bits <= dyn_bits) {
            put_bits(&w, final ? 1 : 0, 1);
            put_bits(&w, 1, 2);
            write_symbols(&w, fix, syms, ns);
        } else {
            put_bits(&w, final ? 1 : 0, 1);
            put_bits(&w, 2, 2);
            put_bits(&w, (uint32_t)(dyn->hlit - 257), 5);
            put_bits(&w, (uint32_t)(dyn->hdist - 1), 5);
            put_bits(&w, (uint32_t)(dyn->hclen - 4), 4);
            for (int i = 0; i < dyn->hclen; ++i) put_bits(&w, dyn->clen[clen_order[i]], 3);
            for (int i = 0; i < dyn->nrle; ++i) {
                uint8_t c = dyn->rle[i];
                put_code(&w, dyn->ccode[c], dyn->clen[c]);
                if (c == 16) put_bits(&w, dyn->rle_extra[i], 2);
                else if (c == 17) put_bits(&w, dyn->rle_extra[i], 3);
                else if (c == 18) put_bits(&w, dyn->rle_extra[i], 7);
            }
            write_symbols(&w, dyn, syms, ns);
        }
    }
    if (segment && err == PROVEN_OK) write_stored(&w, NULL, 0, false);
    align(&w);
    rp_mem_free(alloc, head);
    rp_mem_free(alloc, prev);
    rp_mem_free(alloc, syms);
    rp_mem_free(alloc, dyn);
    rp_mem_free(alloc, fix);
    if (err != PROVEN_OK) {
        rp_buf_free(&w.b);
        return err;
    }
    return rp_buf_take(&w.b, out, out_len);
}

proven_err_t rp_deflate(proven_allocator_t alloc, const uint8_t *in, size_t n, int level, uint8_t **out, size_t *out_len) {
    return run(alloc, in, n, level, false, out, out_len);
}

proven_err_t rp_deflate_segment(proven_allocator_t alloc, const uint8_t *in, size_t n, int level, uint8_t **out, size_t *out_len) {
    return run(alloc, in, n, level, true, out, out_len);
}

// ---- decoder ---------------------------------------------------------------------------------

typedef struct {
    const uint8_t *in;
    size_t         len;
    size_t         bit;     // position in bits
    bool           bad;
} reader_t;

static uint32_t get_bits(reader_t *r, unsigned n) {
    uint32_t v = 0;
    for (unsigned k = 0; k < n; ++k) {
        if (r->bit >= r->len * 8) {
            r->bad = true;
            return 0;
        }
        v |= (uint32_t)((r->in[r->bit >> 3] >> (r->bit & 7)) & 1u) << k;
        r->bit++;
    }
    return v;
}

typedef struct {
    uint16_t count[16];
    uint16_t symbol[LITLEN];
} huff_t;

// Builds a canonical decoder; refuses over-subscribed codes (incomplete ones decode until an
// unused code is met, which is then an error).
static bool huff_build(huff_t *h, const uint8_t *lens, int n) {
    memset(h->count, 0, sizeof h->count);
    for (int i = 0; i < n; ++i) h->count[lens[i]]++;
    h->count[0] = 0;
    int left = 1;
    for (int b = 1; b < 16; ++b) {
        left <<= 1;
        left -= h->count[b];
        if (left < 0) return false;
    }
    uint16_t offs[16];
    offs[1] = 0;
    for (int b = 1; b < 15; ++b) offs[b + 1] = (uint16_t)(offs[b] + h->count[b]);
    for (int i = 0; i < n; ++i) {
        if (lens[i]) h->symbol[offs[lens[i]]++] = (uint16_t)i;
    }
    return true;
}

static int huff_decode(reader_t *r, const huff_t *h) {
    int code = 0, first = 0, index = 0;
    for (int b = 1; b < 16; ++b) {
        code |= (int)get_bits(r, 1);
        if (r->bad) return -1;
        int count = h->count[b];
        if (code - first < count) return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

proven_err_t rp_inflate(const uint8_t *in, size_t in_len, uint8_t *buf, size_t cap, size_t *pos, size_t *used) {
    if ((in == NULL && in_len != 0) || buf == NULL || pos == NULL || *pos > cap) return PROVEN_ERR_INVALID_ARG;
    reader_t r = { in, in_len, 0, false };
    size_t o = *pos;
    huff_t lit, dst;
    bool final = false;
    while (!final) {
        final = get_bits(&r, 1) != 0;
        uint32_t type = get_bits(&r, 2);
        if (r.bad || type == 3) return PROVEN_ERR_INVALID_FORMAT;
        if (type == 0) {
            r.bit = (r.bit + 7) & ~(size_t)7;
            uint32_t len = get_bits(&r, 16), nlen = get_bits(&r, 16);
            if (r.bad || (len ^ 0xFFFF) != nlen) return PROVEN_ERR_INVALID_FORMAT;
            size_t at = r.bit / 8;
            if (at + len > in_len || o + len > cap) return PROVEN_ERR_INVALID_FORMAT;
            memcpy(buf + o, in + at, len);
            o += len;
            r.bit += (size_t)len * 8;
            continue;
        }
        uint8_t lens[LITLEN + DISTS];
        int hlit = 288, hdist = 30;
        if (type == 1) {
            for (int i = 0; i < LITLEN; ++i) lens[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
            for (int i = 0; i < DISTS; ++i) lens[LITLEN + i] = 5;
            huff_build(&lit, lens, LITLEN);
            huff_build(&dst, lens + LITLEN, DISTS);
        } else {
            hlit = (int)get_bits(&r, 5) + 257;
            hdist = (int)get_bits(&r, 5) + 1;
            int hclen = (int)get_bits(&r, 4) + 4;
            if (r.bad || hlit > 286 || hdist > 30) return PROVEN_ERR_INVALID_FORMAT;
            uint8_t cl[19] = { 0 };
            for (int i = 0; i < hclen; ++i) cl[clen_order[i]] = (uint8_t)get_bits(&r, 3);
            huff_t ch;
            if (r.bad || !huff_build(&ch, cl, 19)) return PROVEN_ERR_INVALID_FORMAT;
            uint8_t all[LITLEN + DISTS];
            for (int i = 0; i < hlit + hdist;) {
                int s = huff_decode(&r, &ch);
                if (s < 0) return PROVEN_ERR_INVALID_FORMAT;
                if (s < 16) {
                    all[i++] = (uint8_t)s;
                    continue;
                }
                int rep, val = 0;
                if (s == 16) {
                    if (i == 0) return PROVEN_ERR_INVALID_FORMAT;
                    val = all[i - 1];
                    rep = 3 + (int)get_bits(&r, 2);
                } else if (s == 17) {
                    rep = 3 + (int)get_bits(&r, 3);
                } else {
                    rep = 11 + (int)get_bits(&r, 7);
                }
                if (r.bad || i + rep > hlit + hdist) return PROVEN_ERR_INVALID_FORMAT;
                while (rep--) all[i++] = (uint8_t)val;
            }
            if (all[256] == 0) return PROVEN_ERR_INVALID_FORMAT;
            memset(lens, 0, sizeof lens);
            memcpy(lens, all, (size_t)hlit);
            memcpy(lens + LITLEN, all + hlit, (size_t)hdist);
            if (!huff_build(&lit, lens, LITLEN) || !huff_build(&dst, lens + LITLEN, DISTS)) return PROVEN_ERR_INVALID_FORMAT;
        }
        for (;;) {
            int s = huff_decode(&r, &lit);
            if (s < 0) return PROVEN_ERR_INVALID_FORMAT;
            if (s < 256) {
                if (o >= cap) return PROVEN_ERR_INVALID_FORMAT;
                buf[o++] = (uint8_t)s;
                continue;
            }
            if (s == 256) break;
            s -= 257;
            if (s >= 29) return PROVEN_ERR_INVALID_FORMAT;
            size_t len = len_base[s] + get_bits(&r, len_extra[s]);
            int ds = huff_decode(&r, &dst);
            if (ds < 0 || ds >= 30) return PROVEN_ERR_INVALID_FORMAT;
            size_t d = dist_base[ds] + get_bits(&r, dist_extra[ds]);
            if (r.bad || d > o || o + len > cap) return PROVEN_ERR_INVALID_FORMAT;
            for (size_t k = 0; k < len; ++k, ++o) buf[o] = buf[o - d];
        }
    }
    *pos = o;
    if (used) *used = (r.bit + 7) / 8;
    return PROVEN_OK;
}
