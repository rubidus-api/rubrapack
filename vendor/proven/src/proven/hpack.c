#include "proven/hpack.h"
#include "hpack_tables.h"

/*
 * HPACK (RFC 7541).
 *
 * Both halves keep the same thing: a dynamic table, newest entry first, whose size is the sum
 * of each entry's name, value and 32. Adding an entry evicts the oldest until the new one
 * fits; one that is larger than the whole table empties it and is not added.
 *
 * The table here is one allocation: an array of entries used as a ring, and behind it the
 * bytes of the names and values, appended in order and moved to the front when the tail is
 * reached. Its capacity follows the limit, so it is made on the first insertion and remade
 * only when the limit grows.
 */

#define HPACK_INT_MAX_CONTINUATION 5u      /* bytes after the prefix: values to 2^32 and no further */

typedef struct { proven_u32 at, name_len, value_len; } proven_hp_entry_t;

typedef struct {
    proven_allocator_t alloc;
    proven_hp_entry_t *entries;               /* a ring of `slots` */
    proven_byte_t *bytes;              /* `cap` bytes; live strings lie in [lo, hi) */
    proven_size_t slots, cap;
    proven_size_t head, count;         /* the oldest entry, and how many */
    proven_size_t lo, hi;
    proven_size_t size;                /* by the RFC's measure */
    proven_size_t max;                 /* the size in force now */
} proven_hp_table_t;

static void hp_table_free(proven_hp_table_t *t) {
    if (t->entries) t->alloc.free_fn(t->alloc.ctx, t->entries);
    t->entries = (void *)0;
    t->bytes = (void *)0;
    t->slots = t->cap = t->head = t->count = t->lo = t->hi = t->size = 0;
}

static proven_hp_entry_t *hp_at(const proven_hp_table_t *t, proven_size_t newest_first) {
    /* Index 0 is the newest: the last one added. */
    proven_size_t at = t->head + t->count - 1 - newest_first;          /* under twice `slots`: no division */
    return &t->entries[at >= t->slots ? at - t->slots : at];
}

static void hp_evict_one(proven_hp_table_t *t) {
    proven_hp_entry_t *e = &t->entries[t->head];
    t->size -= (proven_size_t)e->name_len + e->value_len + PROVEN_HPACK_ENTRY_OVERHEAD;
    t->lo = (proven_size_t)e->at + e->name_len + e->value_len;
    t->head = (t->head + 1) % t->slots;
    t->count--;
    if (t->count == 0) { t->lo = 0; t->hi = 0; t->head = 0; }
}

static void hp_table_shrink_to(proven_hp_table_t *t, proven_size_t max) {
    while (t->count > 0 && t->size > max) hp_evict_one(t);
    if (t->count == 0 && t->max == 0) hp_table_free(t);       /* a table that may hold nothing holds no memory */
}

/* Room for `add_slots` more entries holding `add_bytes` more bytes of names and values. What
 * is in the table is kept. The table grows as it is used - a connection whose peer sends a
 * few short fields keeps a few hundred bytes, not the 4,096 the protocol allows it - and never
 * past what `max` can hold. */
static proven_err_t hp_table_room(proven_hp_table_t *t, proven_size_t add_slots, proven_size_t add_bytes) {
    const proven_size_t live = t->hi - t->lo;
    const proven_size_t top_slots = t->max / PROVEN_HPACK_ENTRY_OVERHEAD + 1;
    proven_size_t slots = t->slots, cap = t->cap;
    proven_size_t want_slots = t->count + add_slots, want_bytes = live + add_bytes;
    if (want_slots > top_slots) want_slots = top_slots;
    if (want_bytes > t->max) want_bytes = t->max;
    if (want_slots > slots) {
        slots = slots ? slots : 8;
        while (slots < want_slots) slots *= 2;
        if (slots > top_slots) slots = top_slots;
    }
    if (want_bytes > cap) {
        cap = cap ? cap : 256;
        while (cap < want_bytes) cap *= 2;
        if (cap > t->max) cap = t->max;
    }
    if (t->entries && slots == t->slots && cap == t->cap) return PROVEN_OK;
    if (t->max > (proven_size_t)1 << 30) return PROVEN_ERR_NOMEM;
    proven_size_t head_bytes = slots * sizeof(proven_hp_entry_t);
    proven_result_mem_mut_t m = t->alloc.alloc_fn(t->alloc.ctx, head_bytes + cap, alignof(proven_hp_entry_t));
    if (m.err != PROVEN_OK) return PROVEN_ERR_NOMEM;
    proven_hp_entry_t *entries = (proven_hp_entry_t *)(void *)m.value.ptr;
    proven_byte_t *bytes = m.value.ptr + head_bytes;
    proven_size_t at = 0;
    for (proven_size_t i = 0; i < t->count; ++i) {
        proven_hp_entry_t e = t->entries[(t->head + i) % t->slots];
        proven_size_t n = (proven_size_t)e.name_len + e.value_len;
        for (proven_size_t k = 0; k < n; ++k) bytes[at + k] = t->bytes[e.at + k];
        entries[i] = (proven_hp_entry_t){ .at = (proven_u32)at, .name_len = e.name_len, .value_len = e.value_len };
        at += n;
    }
    if (t->entries) t->alloc.free_fn(t->alloc.ctx, t->entries);
    t->entries = entries;
    t->bytes = bytes;
    t->slots = slots;
    t->cap = cap;
    t->head = 0;
    t->lo = 0;
    t->hi = at;
    return PROVEN_OK;
}

/* Add an entry (RFC 7541, section 4.4). `name` and `value` must not point into the table. */
static proven_err_t hp_table_add(proven_hp_table_t *t, proven_u8str_view_t name, proven_u8str_view_t value) {
    proven_size_t need = name.size + value.size + PROVEN_HPACK_ENTRY_OVERHEAD;
    if (need > t->max) {
        /* Larger than the table: it empties the table and is not added. Not an error. */
        hp_table_shrink_to(t, 0);
        return PROVEN_OK;
    }
    hp_table_shrink_to(t, t->max - need);
    proven_size_t n = name.size + value.size;
    proven_err_t e = hp_table_room(t, 1, n);
    if (e != PROVEN_OK) return e;
    if (t->cap - t->hi < n) {
        /* The tail is reached: move what is live to the front. */
        proven_size_t live = t->hi - t->lo;
        for (proven_size_t i = 0; i < live; ++i) t->bytes[i] = t->bytes[t->lo + i];
        for (proven_size_t i = 0; i < t->count; ++i) t->entries[(t->head + i) % t->slots].at -= (proven_u32)t->lo;
        t->lo = 0;
        t->hi = live;
    }
    proven_hp_entry_t *slot = &t->entries[(t->head + t->count) % t->slots];
    *slot = (proven_hp_entry_t){ .at = (proven_u32)t->hi, .name_len = (proven_u32)name.size, .value_len = (proven_u32)value.size };
    for (proven_size_t i = 0; i < name.size; ++i) t->bytes[t->hi + i] = name.ptr[i];
    for (proven_size_t i = 0; i < value.size; ++i) t->bytes[t->hi + name.size + i] = value.ptr[i];
    t->hi += n;
    t->count++;
    t->size += need;
    return PROVEN_OK;
}

static proven_u8str_view_t hp_name(const proven_hp_table_t *t, const proven_hp_entry_t *e) {
    return (proven_u8str_view_t){ .ptr = t->bytes + e->at, .size = e->name_len };
}
static proven_u8str_view_t hp_value(const proven_hp_table_t *t, const proven_hp_entry_t *e) {
    return (proven_u8str_view_t){ .ptr = t->bytes + e->at + e->name_len, .size = e->value_len };
}

static proven_u8str_view_t hp_static_name(proven_size_t i) {
    return (proven_u8str_view_t){ .ptr = (const proven_byte_t *)hpack_static[i].name, .size = hpack_static[i].name_len };
}
static proven_u8str_view_t hp_static_value(proven_size_t i) {
    return (proven_u8str_view_t){ .ptr = (const proven_byte_t *)hpack_static[i].value, .size = hpack_static[i].value_len };
}

static bool hp_eq(proven_u8str_view_t a, proven_u8str_view_t b) {
    if (a.size != b.size) return false;
    for (proven_size_t i = 0; i < a.size; ++i) if (a.ptr[i] != b.ptr[i]) return false;
    return true;
}

// -----------------------------------------------------------------------------
// Huffman coding
// -----------------------------------------------------------------------------

proven_size_t proven_hpack_huffman_size(proven_u8str_view_t text) {
    proven_u64 bits = 0;
    for (proven_size_t i = 0; i < text.size; ++i) bits += hpack_huffman[text.ptr[i]].bits;
    return (proven_size_t)((bits + 7) / 8);
}

/* The coded form of `n` bytes at `out`, if it takes no more than `limit` bytes: its length.
 * Otherwise 0, with `out` written to somewhere within `limit`. One pass: the code's bits are
 * gathered in 64 and leave four bytes at a time (a code is at most 30 bits long). */
static proven_size_t hp_huffman_put(const proven_byte_t *p, proven_size_t n, proven_byte_t *out, proven_size_t limit) {
    proven_size_t at = 0;
    proven_u64 acc = 0;
    unsigned held = 0;
    for (proven_size_t i = 0; i < n; ++i) {
        const unsigned bits = hpack_huffman[p[i]].bits;
        acc = (acc << bits) | hpack_huffman[p[i]].code;
        held += bits;
        if (held >= 32) {
            if (limit - at < 4) return 0;
            held -= 32;
            const proven_u32 w = (proven_u32)(acc >> held);
            out[at] = (proven_byte_t)(w >> 24); out[at + 1] = (proven_byte_t)(w >> 16); out[at + 2] = (proven_byte_t)(w >> 8); out[at + 3] = (proven_byte_t)w;
            at += 4;
        }
    }
    if (limit - at < (held + 7) / 8) return 0;
    while (held >= 8) { held -= 8; out[at++] = (proven_byte_t)(acc >> held); }
    /* The rest of the last byte is filled with the first bits of EOS: all ones. */
    if (held > 0) out[at++] = (proven_byte_t)((acc << (8 - held)) | (0xffu >> held));
    return at;
}

proven_err_t proven_hpack_huffman_encode(proven_u8str_view_t text, proven_mem_mut_t out, proven_size_t *len) {
    if (!len || (text.size > 0 && !text.ptr) || (out.size > 0 && !out.ptr) || *len > out.size) return PROVEN_ERR_INVALID_ARG;
    proven_size_t need = proven_hpack_huffman_size(text);
    if (out.size - *len < need) return PROVEN_ERR_OUT_OF_BOUNDS;
    *len += hp_huffman_put(text.ptr, text.size, out.ptr + *len, need);
    return PROVEN_OK;
}

proven_err_t proven_hpack_huffman_decode(proven_mem_view_t coded, proven_mem_mut_t out, proven_size_t *len) {
    if (!len || (coded.size > 0 && !coded.ptr) || (out.size > 0 && !out.ptr) || *len > out.size) return PROVEN_ERR_INVALID_ARG;
    proven_size_t at = *len, i = 0;
    proven_u64 acc = 0;                /* bits read and not yet used, the oldest highest */
    unsigned bits = 0;
    for (;;) {
        while (bits <= 56 && i < coded.size) { acc = (acc << 8) | coded.ptr[i++]; bits += 8; }
        if (bits == 0) break;
        proven_u16 symbol = 0;
        unsigned used = 0;
        /* Most codes are eight bits or fewer: the next eight bits name the symbol at once.
         * (With fewer than eight left they are filled out with ones, which begin no code.) */
        const unsigned next = bits >= 8 ? (unsigned)(acc >> (bits - 8)) & 0xffu
                                        : (((unsigned)acc << (8 - bits)) | (0xffu >> bits)) & 0xffu;
        const proven_u16 quick = hpack_huffman_fast[next];
        if (quick != 0 && (quick & 15u) <= bits) {
            symbol = (proven_u16)(quick >> 4);
            used = quick & 15u;
        } else {
            /* A longer code: by length, as the canonical code allows. */
            for (unsigned n = 9; n <= 30 && n <= bits; ++n) {
                const proven_u32 code = (proven_u32)(acc >> (bits - n)) & (((proven_u32)1 << n) - 1);
                if (hpack_huffman_count[n] != 0 && code >= hpack_huffman_first[n] && code - hpack_huffman_first[n] < hpack_huffman_count[n]) {
                    symbol = hpack_huffman_sorted[hpack_huffman_offset[n] + (code - hpack_huffman_first[n])];
                    used = n;
                    break;
                }
            }
            if (used == 0) {
                /* No code here. Thirty bits that are none is an error; fewer are the end of the
                 * string, and must then be its padding: under eight bits, all ones (the start
                 * of EOS). */
                if (bits >= 30 || i < coded.size) return PROVEN_ERR_INVALID_FORMAT;
                if (bits > 7 || (acc & (((proven_u64)1 << bits) - 1)) != (((proven_u64)1 << bits) - 1)) return PROVEN_ERR_INVALID_FORMAT;
                break;
            }
        }
        if (symbol == 256) return PROVEN_ERR_INVALID_FORMAT;               /* EOS inside a string */
        if (at == out.size) return PROVEN_ERR_OUT_OF_BOUNDS;
        out.ptr[at++] = (proven_byte_t)symbol;
        bits -= used;
    }
    *len = at;
    return PROVEN_OK;
}

// -----------------------------------------------------------------------------
// Integers and strings (RFC 7541, section 5)
// -----------------------------------------------------------------------------

/* An integer with an N-bit prefix at in[*pos]. False when it runs past the block or is too
 * large. */
static bool hp_int(proven_mem_view_t in, proven_size_t *pos, unsigned prefix, proven_u64 *out) {
    if (*pos >= in.size) return false;
    proven_u64 mask = (1u << prefix) - 1u;
    proven_u64 v = in.ptr[(*pos)++] & mask;
    if (v == mask) {
        unsigned shift = 0;
        for (unsigned i = 0;; ++i) {
            if (i == HPACK_INT_MAX_CONTINUATION || *pos >= in.size) return false;
            proven_byte_t b = in.ptr[(*pos)++];
            v += (proven_u64)(b & 0x7f) << shift;
            shift += 7;
            if ((b & 0x80) == 0) break;
        }
    }
    *out = v;
    return true;
}

static void hp_put_int(proven_byte_t *out, proven_size_t *at, unsigned prefix, proven_byte_t first_bits, proven_u64 v) {
    proven_u64 mask = (1u << prefix) - 1u;
    if (v < mask) { out[(*at)++] = (proven_byte_t)(first_bits | v); return; }
    out[(*at)++] = (proven_byte_t)(first_bits | mask);
    v -= mask;
    while (v >= 128) { out[(*at)++] = (proven_byte_t)(0x80 | (v & 0x7f)); v >>= 7; }
    out[(*at)++] = (proven_byte_t)v;
}

// -----------------------------------------------------------------------------
// The decoder
// -----------------------------------------------------------------------------

struct proven_hpack_decoder {
    proven_hp_table_t table;
    proven_size_t limit;               /* the largest size the peer may set: what this side announced */
    bool update_owed;                  /* the limit went below the table's size: the next block must open with an update */
    bool failed;
};

proven_err_t proven_hpack_decoder_create(proven_allocator_t alloc, proven_size_t max_table_bytes, proven_hpack_decoder_t **out) {
    if (out) *out = (void *)0;
    if (!out || !proven_alloc_is_valid(alloc) || max_table_bytes > (proven_size_t)1 << 30) return PROVEN_ERR_INVALID_ARG;
    proven_result_mem_mut_t m = alloc.alloc_fn(alloc.ctx, sizeof(proven_hpack_decoder_t), alignof(proven_hpack_decoder_t));
    if (m.err != PROVEN_OK) return PROVEN_ERR_NOMEM;
    proven_hpack_decoder_t *d = (proven_hpack_decoder_t *)(void *)m.value.ptr;
    *d = (proven_hpack_decoder_t){ .table = { .alloc = alloc, .max = max_table_bytes }, .limit = max_table_bytes };
    *out = d;
    return PROVEN_OK;
}

void proven_hpack_decoder_destroy(proven_hpack_decoder_t *d) {
    if (!d) return;
    proven_allocator_t a = d->table.alloc;
    hp_table_free(&d->table);
    a.free_fn(a.ctx, d);
}

void proven_hpack_decoder_set_max_table_bytes(proven_hpack_decoder_t *d, proven_size_t max_table_bytes) {
    if (!d || max_table_bytes > (proven_size_t)1 << 30) return;
    d->limit = max_table_bytes;
    /* Lowered below what is in force: the peer's encoder has to say that it followed. */
    if (max_table_bytes < d->table.max) d->update_owed = true;
}

proven_size_t proven_hpack_decoder_table_count(const proven_hpack_decoder_t *d) { return d ? d->table.count : 0; }
proven_size_t proven_hpack_decoder_table_bytes(const proven_hpack_decoder_t *d) { return d ? d->table.size : 0; }

bool proven_hpack_decoder_table_entry(const proven_hpack_decoder_t *d, proven_size_t index, proven_u8str_view_t *name, proven_u8str_view_t *value) {
    if (!d || !name || !value || index >= d->table.count) return false;
    const proven_hp_entry_t *e = hp_at(&d->table, index);
    *name = hp_name(&d->table, e);
    *value = hp_value(&d->table, e);
    return true;
}

/* What one decode call carries along. */
typedef struct {
    proven_mem_view_t in;
    proven_size_t pos;
    proven_mem_mut_t scratch;
    proven_size_t used;                /* of scratch */
    bool over;                         /* the list passed a limit: nothing more is given out, but the block is read on */
} proven_hp_run_t;

/* Copy `s` into the scratch and return the copy. False when there is no room. */
static bool hp_keep(proven_hp_run_t *r, proven_u8str_view_t s, proven_u8str_view_t *out) {
    if (s.size > r->scratch.size - r->used) return false;
    for (proven_size_t i = 0; i < s.size; ++i) r->scratch.ptr[r->used + i] = s.ptr[i];
    *out = (proven_u8str_view_t){ .ptr = r->scratch.ptr + r->used, .size = s.size };
    r->used += s.size;
    return true;
}

/* A string literal (section 5.2). PROVEN_ERR_INVALID_FORMAT, or PROVEN_ERR_OUT_OF_BOUNDS when
 * it is Huffman-coded and the scratch has no room for it. */
static proven_err_t hp_string(proven_hp_run_t *r, proven_u8str_view_t *out) {
    if (r->pos >= r->in.size) return PROVEN_ERR_INVALID_FORMAT;
    bool huffman = (r->in.ptr[r->pos] & 0x80) != 0;
    proven_u64 n = 0;
    if (!hp_int(r->in, &r->pos, 7, &n) || n > r->in.size - r->pos) return PROVEN_ERR_INVALID_FORMAT;
    proven_mem_view_t raw = { .ptr = r->in.ptr + r->pos, .size = (proven_size_t)n };
    r->pos += (proven_size_t)n;
    if (!huffman) {
        *out = (proven_u8str_view_t){ .ptr = raw.ptr, .size = raw.size };
        return PROVEN_OK;
    }
    proven_size_t at = r->used;
    proven_err_t e = proven_hpack_huffman_decode(raw, r->scratch, &at);
    if (e != PROVEN_OK) return e;
    *out = (proven_u8str_view_t){ .ptr = r->scratch.ptr + r->used, .size = at - r->used };
    r->used = at;
    return PROVEN_OK;
}

proven_err_t proven_hpack_decode(proven_hpack_decoder_t *d, proven_mem_view_t block,
                                 proven_http_header_t *headers, proven_size_t header_cap,
                                 proven_mem_mut_t scratch, proven_size_t max_list_bytes, proven_size_t *count) {
    return proven_hpack_decode_marked(d, block, headers, (void *)0, header_cap, scratch, max_list_bytes, count);
}

proven_err_t proven_hpack_decode_marked(proven_hpack_decoder_t *d, proven_mem_view_t block,
                                        proven_http_header_t *headers, bool *never_indexed, proven_size_t header_cap,
                                        proven_mem_mut_t scratch, proven_size_t max_list_bytes, proven_size_t *count) {
    if (count) *count = 0;
    if (!d || !count || (block.size > 0 && !block.ptr) || (header_cap > 0 && !headers) || (scratch.size > 0 && !scratch.ptr)) return PROVEN_ERR_INVALID_ARG;
    if (d->failed) return PROVEN_ERR_INVALID_FORMAT;
    proven_hp_table_t *t = &d->table;
    proven_hp_run_t r = { .in = block, .scratch = scratch };
    proven_size_t n = 0;
    proven_u64 list = 0;
    bool fields_begun = false;
    proven_err_t e = PROVEN_ERR_INVALID_FORMAT;

    while (r.pos < block.size) {
        proven_byte_t b = block.ptr[r.pos];
        proven_u8str_view_t name = { 0 }, value = { 0 };
        bool insert = false, never = false;
        /* Once the list is over a limit nothing more is given out, but the block is read to
         * its end so that the table learns what it must: each field is then worked on at the
         * start of the scratch. */
        const proven_size_t field_at = r.pos;
        if (r.over) r.used = 0;
        e = PROVEN_ERR_INVALID_FORMAT;

        if ((b & 0xe0) == 0x20) {
            /* A dynamic table size update: only before the first field, and within the limit. */
            proven_u64 size = 0;
            if (fields_begun || !hp_int(block, &r.pos, 5, &size) || size > d->limit) goto fail;
            t->max = (proven_size_t)size;
            hp_table_shrink_to(t, t->max);
            d->update_owed = false;
            continue;
        }
        if (d->update_owed) goto fail;             /* the limit was lowered and the peer did not follow */
        fields_begun = true;

        if ((b & 0x80) != 0) {
            /* Indexed: both from a table. */
            proven_u64 index = 0;
            if (!hp_int(block, &r.pos, 7, &index) || index == 0) goto fail;
            if (index <= HPACK_STATIC_COUNT) {
                name = hp_static_name((proven_size_t)index - 1);
                value = hp_static_value((proven_size_t)index - 1);
            } else {
                if (index - HPACK_STATIC_COUNT > t->count) goto fail;
                const proven_hp_entry_t *en = hp_at(t, (proven_size_t)(index - HPACK_STATIC_COUNT - 1));
                /* Copied: a later field of this block may evict it. */
                if (!hp_keep(&r, hp_name(t, en), &name) || !hp_keep(&r, hp_value(t, en), &value)) goto no_room;
            }
        } else {
            /* A literal: with indexing (01), without (0000), or never indexed (0001). */
            unsigned prefix = (b & 0x40) != 0 ? 6u : 4u;
            insert = (b & 0x40) != 0;
            never = (b & 0xf0) == 0x10;
            proven_u64 index = 0;
            if (!hp_int(block, &r.pos, prefix, &index)) goto fail;
            if (index == 0) {
                e = hp_string(&r, &name);
                if (e == PROVEN_ERR_OUT_OF_BOUNDS) goto no_room;
                if (e != PROVEN_OK) goto fail;
            } else if (index <= HPACK_STATIC_COUNT) {
                name = hp_static_name((proven_size_t)index - 1);
            } else {
                if (index - HPACK_STATIC_COUNT > t->count) goto fail;
                const proven_hp_entry_t *en = hp_at(t, (proven_size_t)(index - HPACK_STATIC_COUNT - 1));
                if (!hp_keep(&r, hp_name(t, en), &name)) goto no_room;
            }
            e = hp_string(&r, &value);
            if (e == PROVEN_ERR_OUT_OF_BOUNDS) goto no_room;
            if (e != PROVEN_OK) goto fail;
            e = PROVEN_ERR_INVALID_FORMAT;
        }

        if (insert) {
            proven_err_t te = hp_table_add(t, name, value);
            if (te != PROVEN_OK) { e = te; goto fail; }
        }
        list += (proven_u64)name.size + value.size + PROVEN_HPACK_ENTRY_OVERHEAD;
        if ((max_list_bytes != 0 && list > max_list_bytes) || n == header_cap) r.over = true;
        if (!r.over) {
            if (never_indexed) never_indexed[n] = never;
            headers[n++] = (proven_http_header_t){ .name = name, .value = value };
        }
        continue;

    no_room:
        /* The scratch is full. The first time, that is a list too large to give out: read
         * this field again with the whole scratch to work in. If it does not fit even so,
         * the table cannot be kept in step. */
        if (r.over) { e = PROVEN_ERR_INVALID_FORMAT; goto fail; }
        r.over = true;
        r.pos = field_at;
    }
    if (r.over) return PROVEN_ERR_OUT_OF_BOUNDS;   /* read to its end: the table is in step */
    *count = n;
    return PROVEN_OK;

fail:
    d->failed = true;
    return e == PROVEN_ERR_NOMEM ? PROVEN_ERR_NOMEM : PROVEN_ERR_INVALID_FORMAT;
}

// -----------------------------------------------------------------------------
// The encoder
// -----------------------------------------------------------------------------

struct proven_hpack_encoder {
    proven_hp_table_t table;
    proven_size_t own_max;             /* the most this encoder will use */
    proven_size_t peer_max;            /* the most the peer allows */
    proven_size_t announce_low;        /* a size to announce first: the smallest in force since the last block */
    bool announce;                     /* the size changed since the last block */
};

proven_err_t proven_hpack_encoder_create(proven_allocator_t alloc, proven_size_t max_table_bytes, proven_hpack_encoder_t **out) {
    if (out) *out = (void *)0;
    if (!out || !proven_alloc_is_valid(alloc) || max_table_bytes > (proven_size_t)1 << 30) return PROVEN_ERR_INVALID_ARG;
    proven_result_mem_mut_t m = alloc.alloc_fn(alloc.ctx, sizeof(proven_hpack_encoder_t), alignof(proven_hpack_encoder_t));
    if (m.err != PROVEN_OK) return PROVEN_ERR_NOMEM;
    proven_hpack_encoder_t *c = (proven_hpack_encoder_t *)(void *)m.value.ptr;
    proven_size_t start = max_table_bytes < PROVEN_HPACK_DEFAULT_TABLE_BYTES ? max_table_bytes : PROVEN_HPACK_DEFAULT_TABLE_BYTES;
    *c = (proven_hpack_encoder_t){ .table = { .alloc = alloc, .max = start }, .own_max = max_table_bytes, .peer_max = PROVEN_HPACK_DEFAULT_TABLE_BYTES };
    /* The peer assumes the default until told: an encoder that uses less says so at once. */
    if (start != PROVEN_HPACK_DEFAULT_TABLE_BYTES) { c->announce = true; c->announce_low = start; }
    *out = c;
    return PROVEN_OK;
}

void proven_hpack_encoder_destroy(proven_hpack_encoder_t *c) {
    if (!c) return;
    proven_allocator_t a = c->table.alloc;
    hp_table_free(&c->table);
    a.free_fn(a.ctx, c);
}

void proven_hpack_encoder_set_peer_table_bytes(proven_hpack_encoder_t *c, proven_size_t peer_max_table_bytes) {
    if (!c) return;
    c->peer_max = peer_max_table_bytes;
    proven_size_t now = c->own_max < peer_max_table_bytes ? c->own_max : peer_max_table_bytes;
    if (now == c->table.max) return;
    /* The smallest size in force between two blocks is announced first, so that the peer
     * evicts what this side evicts; then the size that stands. */
    if (!c->announce || now < c->announce_low) c->announce_low = now;
    c->announce = true;
    c->table.max = now;
    hp_table_shrink_to(&c->table, now);
}

static proven_size_t hp_int_size(void) { return 1 + HPACK_INT_MAX_CONTINUATION; }

proven_size_t proven_hpack_encode_bound(const proven_http_header_t *headers, proven_size_t count) {
    proven_size_t n = 2 * hp_int_size();           /* two size updates */
    for (proven_size_t i = 0; i < count && headers; ++i) n += 1 + 2 * hp_int_size() + headers[i].name.size + headers[i].value.size;
    return n;
}

static void hp_put_string(proven_byte_t *out, proven_size_t *at, proven_size_t cap, proven_u8str_view_t s) {
    (void)cap;
    if (s.size >= 2 && s.size <= 127) {
        /* Coded only if that is shorter, and a shorter form of so few bytes has a length of
         * one byte: it is coded in place behind that byte, once, and given up where it stops
         * being shorter. */
        const proven_size_t coded = hp_huffman_put(s.ptr, s.size, out + *at + 1, s.size - 1);
        if (coded != 0) { out[*at] = (proven_byte_t)(0x80u | coded); *at += 1 + coded; return; }
    } else if (s.size > 127) {
        const proven_size_t coded = proven_hpack_huffman_size(s);
        if (coded < s.size) {
            hp_put_int(out, at, 7, 0x80, coded);
            *at += hp_huffman_put(s.ptr, s.size, out + *at, coded);
            return;
        }
    }
    hp_put_int(out, at, 7, 0x00, s.size);
    for (proven_size_t i = 0; i < s.size; ++i) out[(*at)++] = s.ptr[i];
}

/* Fields whose value this encoder does not put in a table unasked: 2 for "never indexed by
 * anyone", 1 for "not indexed here" - the secrets a table would let a peer probe for, and
 * the fields whose value is seldom the same twice on a connection, which would only push out
 * of the table what is. All of these names are in the static table. */
static int hp_reserve_kind(proven_u8str_view_t name) {
    /* By length first: most names are of a length none of these has, and are not compared. */
#define HP_IS(text) hp_eq(name, (proven_u8str_view_t){ .ptr = (const proven_byte_t *)(text), .size = sizeof(text) - 1 })
    switch (name.size) {
        case 3: return HP_IS("age") ? 1 : 0;
        case 4: return HP_IS("etag") ? 1 : 0;
        case 6: return HP_IS("cookie") ? 1 : 0;
        case 10: return HP_IS("set-cookie") ? 1 : 0;
        case 13: return HP_IS("authorization") ? 2 : HP_IS("content-range") || HP_IS("if-none-match") ? 1 : 0;
        case 14: return HP_IS("content-length") ? 1 : 0;
        case 17: return HP_IS("if-modified-since") ? 1 : 0;
        case 19: return HP_IS("proxy-authorization") ? 2 : 0;
        default: return 0;
    }
#undef HP_IS
}

proven_err_t proven_hpack_encode(proven_hpack_encoder_t *c, const proven_http_header_t *headers, proven_size_t count,
                                 const bool *sensitive, proven_mem_mut_t out, proven_size_t *len) {
    if (!c || !len || (count > 0 && !headers) || (out.size > 0 && !out.ptr) || *len > out.size) return PROVEN_ERR_INVALID_ARG;
    for (proven_size_t i = 0; i < count; ++i) {
        if ((headers[i].name.size > 0 && !headers[i].name.ptr) || (headers[i].value.size > 0 && !headers[i].value.ptr)) return PROVEN_ERR_INVALID_ARG;
    }
    if (out.size - *len < proven_hpack_encode_bound(headers, count)) return PROVEN_ERR_OUT_OF_BOUNDS;
    proven_hp_table_t *t = &c->table;
    /* The one thing that can fail later is the table's allocation: make it now, so that a
     * block is either written whole or not begun. */
    if (t->max > 0 && count > 0) {
        proven_size_t bytes = 0;
        for (proven_size_t i = 0; i < count; ++i) bytes += headers[i].name.size + headers[i].value.size;
        proven_err_t e = hp_table_room(t, count, bytes);
        if (e != PROVEN_OK) return e;
    }
    proven_size_t at = *len;
    if (c->announce) {
        if (c->announce_low < t->max) hp_put_int(out.ptr, &at, 5, 0x20, c->announce_low);
        hp_put_int(out.ptr, &at, 5, 0x20, t->max);
        c->announce = false;
    }
    for (proven_size_t i = 0; i < count; ++i) {
        proven_u8str_view_t name = headers[i].name, value = headers[i].value;
        int reserve = hp_reserve_kind(name);
        if (sensitive && sensitive[i]) reserve = 2;

        /* Where the tables have it: the whole field, or at least its name. */
        proven_size_t whole = 0, by_name = 0;
        if (name.size <= HPACK_STATIC_LONGEST_NAME) {
            /* Only the static names as long as this one are compared with it. */
            for (proven_size_t j = hpack_static_by_len[name.size]; j < hpack_static_by_len[name.size + 1]; ++j) {
                const proven_size_t first = hpack_static_names[j].first;
                if (!hp_eq(hp_static_name(first), name)) continue;
                by_name = first + 1;
                for (proven_size_t k = first; k < first + hpack_static_names[j].count; ++k) {
                    if (hp_eq(hp_static_value(k), value)) { whole = k + 1; break; }
                }
                break;
            }
        }
        /* (A field this encoder never puts in the table, its name found: nothing to look for.) */
        for (proven_size_t k = 0; k < t->count && whole == 0 && (reserve == 0 || by_name == 0); ++k) {
            const proven_hp_entry_t *en = hp_at(t, k);
            if (!hp_eq(hp_name(t, en), name)) continue;
            if (by_name == 0) by_name = HPACK_STATIC_COUNT + k + 1;
            if (hp_eq(hp_value(t, en), value)) whole = HPACK_STATIC_COUNT + k + 1;
        }
        if (whole != 0 && reserve != 2) {
            hp_put_int(out.ptr, &at, 7, 0x80, whole);
            continue;
        }
        proven_size_t entry = name.size + value.size + PROVEN_HPACK_ENTRY_OVERHEAD;
        bool index = reserve == 0 && entry <= t->max;
        if (index) hp_put_int(out.ptr, &at, 6, 0x40, by_name);
        else hp_put_int(out.ptr, &at, 4, reserve == 2 ? 0x10 : 0x00, by_name);
        if (by_name == 0) hp_put_string(out.ptr, &at, out.size, name);
        hp_put_string(out.ptr, &at, out.size, value);
        /* The table was reserved above and the entry fits: this cannot fail. `name` and
         * `value` are the caller's memory, not the table's. */
        if (index) (void)hp_table_add(t, name, value);
    }
    *len = at;
    return PROVEN_OK;
}
