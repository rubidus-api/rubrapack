#include "proven_internal_http_coding.h"

/*
 * Content codings for the HTTP drivers: when a body is decoded, when a response is
 * compressed, and the small machines that do it over deflate.h and zstd.h.
 */

// -----------------------------------------------------------------------------
// Decoding
// -----------------------------------------------------------------------------

bool proven_http_decoder_wanted_(proven_u16 status, const proven_http_header_t *headers, proven_size_t count, bool zstd, bool br, proven_http_coding_t *coding) {
    /* 206 carries a range of the encoded bytes: a piece of a gzip stream is not one. */
    if (status == 206) return false;
    proven_http_coding_t c = proven_http_content_coding(headers, count);
    if (c != PROVEN_HTTP_CODING_GZIP && c != PROVEN_HTTP_CODING_DEFLATE && !(zstd && c == PROVEN_HTTP_CODING_ZSTD) &&
        !(br && c == PROVEN_HTTP_CODING_BROTLI)) return false;
    *coding = c;
    return true;
}

proven_u8str_view_t proven_http_decoder_accept_(bool zstd, bool br) {
    return zstd && br ? PROVEN_LIT("zstd, br, gzip") : zstd ? PROVEN_LIT("zstd, gzip") : br ? PROVEN_LIT("br, gzip") : PROVEN_LIT("gzip");
}

proven_err_t proven_http_decoder_cfg_check_(bool decompress, bool zstd, bool br) {
    if ((zstd || br) && !decompress) return PROVEN_ERR_INVALID_ARG;
#ifdef PROVEN_NO_BROTLI
    if (br) return PROVEN_ERR_UNSUPPORTED;
#endif
    return PROVEN_OK;
}

void proven_http_decoder_begin_(proven_http_decoder_t_ *d, proven_allocator_t alloc, proven_http_coding_t coding) {
    *d = (proven_http_decoder_t_){ .alloc = alloc, .coding = (proven_u8)coding };
}

void proven_http_decoder_free_(proven_http_decoder_t_ *d) {
    if (!d) return;
    if (d->z) proven_inflate_destroy(d->z);
    d->z = (void *)0;
    if (d->zs) proven_zstd_decoder_destroy(d->zs);
    d->zs = (void *)0;
#ifndef PROVEN_NO_BROTLI
    if (d->br) proven_brotli_decoder_destroy(d->br);
#endif
    d->br = (void *)0;
}

/* `deflate` is a zlib stream by the standard and raw DEFLATE from some servers. A zlib header
 * is method 8, a window of at most 32 KiB, and sixteen bits divisible by 31; a raw stream
 * begins like that only when it opens with a stored block whose padding bits are not zero. */
static bool hc_is_zlib(const proven_byte_t h[2]) {
    return (h[0] & 0x0fu) == 8u && (h[0] >> 4) <= 7u && (((unsigned)h[0] << 8) | h[1]) % 31u == 0u;
}

/* Zstandard: frames follow each other, which the decoder reads on its own. */
static proven_err_t hc_zstd_step(proven_http_decoder_t_ *d, proven_mem_view_t in, proven_size_t *consumed,
                                 proven_mem_mut_t out, proven_size_t *produced) {
    if (!d->zs) {
        if (in.size == 0) return PROVEN_OK;
        proven_err_t e = proven_zstd_decoder_create(d->alloc, PROVEN_ZSTD_WINDOW_DEFAULT, &d->zs);
        if (e != PROVEN_OK) return e;
    }
    for (;;) {
        if (*consumed < in.size) d->any = true;
        proven_size_t used = 0, made = 0;
        bool done = false;
        const proven_mem_view_t src = { .ptr = in.ptr ? in.ptr + *consumed : in.ptr, .size = in.size - *consumed };
        const proven_mem_mut_t dst = { .ptr = out.ptr ? out.ptr + *produced : out.ptr, .size = out.size - *produced };
        proven_err_t e = proven_zstd_decode(d->zs, src, &used, dst, &made, &done);
        if (e != PROVEN_OK) return e == PROVEN_ERR_NOMEM ? e : PROVEN_ERR_INVALID_FORMAT;
        *consumed += used;
        *produced += made;
        d->ended = done;
        if (used == 0 && made == 0) return PROVEN_OK;
    }
}

#ifndef PROVEN_NO_BROTLI
/* Brotli: one stream, and nothing after it. The window is the stream's, 16 MiB at most. */
static proven_err_t hc_brotli_step(proven_http_decoder_t_ *d, proven_mem_view_t in, proven_size_t *consumed,
                                   proven_mem_mut_t out, proven_size_t *produced) {
    if (!d->br) {
        if (in.size == 0) return PROVEN_OK;
        proven_err_t e = proven_brotli_decoder_create(d->alloc, PROVEN_BROTLI_WINDOW_DEFAULT, &d->br);
        if (e != PROVEN_OK) return e;
    }
    for (;;) {
        if (*consumed < in.size) {
            if (d->ended) return PROVEN_ERR_INVALID_FORMAT;
            d->any = true;
        }
        proven_size_t used = 0, made = 0;
        bool done = false;
        const proven_mem_view_t src = { .ptr = in.ptr ? in.ptr + *consumed : in.ptr, .size = in.size - *consumed };
        const proven_mem_mut_t dst = { .ptr = out.ptr ? out.ptr + *produced : out.ptr, .size = out.size - *produced };
        proven_err_t e = proven_brotli_decode(d->br, src, &used, dst, &made, &done);
        if (e != PROVEN_OK) return e == PROVEN_ERR_NOMEM ? e : PROVEN_ERR_INVALID_FORMAT;
        *consumed += used;
        *produced += made;
        d->ended = done;
        if (used == 0 && made == 0) return done && *consumed < in.size ? PROVEN_ERR_INVALID_FORMAT : PROVEN_OK;
    }
}
#endif

proven_err_t proven_http_decoder_step_(proven_http_decoder_t_ *d, proven_mem_view_t in, proven_size_t *consumed,
                                       proven_mem_mut_t out, proven_size_t *produced) {
    *consumed = 0;
    *produced = 0;
    if (d->coding == PROVEN_HTTP_CODING_ZSTD) return hc_zstd_step(d, in, consumed, out, produced);
#ifndef PROVEN_NO_BROTLI
    if (d->coding == PROVEN_HTTP_CODING_BROTLI) return hc_brotli_step(d, in, consumed, out, produced);
#endif
    for (;;) {
        if (!d->z) {
            proven_deflate_format_t format = PROVEN_DEFLATE_GZIP;
            if (d->coding == PROVEN_HTTP_CODING_DEFLATE) {
                while (d->held < 2 && *consumed < in.size) d->hold[d->held++] = in.ptr[(*consumed)++];
                if (d->held > 0) d->any = true;
                if (d->held < 2) return PROVEN_OK;
                format = hc_is_zlib(d->hold) ? PROVEN_DEFLATE_ZLIB : PROVEN_DEFLATE_RAW;
            } else if (*consumed == in.size) {
                return PROVEN_OK;
            }
            proven_err_t e = proven_inflate_create(d->alloc, format, &d->z);
            if (e != PROVEN_OK) return e;
        }
        /* The two bytes that were held go in first. */
        bool from_hold = d->held_pos < d->held;
        proven_mem_view_t src = from_hold ? (proven_mem_view_t){ .ptr = d->hold + d->held_pos, .size = (proven_size_t)(d->held - d->held_pos) }
                                          : (proven_mem_view_t){ .ptr = in.ptr + *consumed, .size = in.size - *consumed };
        if (d->ended) {
            if (src.size == 0) return PROVEN_OK;
            /* More bytes after the end of a stream: another gzip member, or nothing valid. */
            if (d->coding != PROVEN_HTTP_CODING_GZIP) return PROVEN_ERR_INVALID_FORMAT;
            proven_inflate_reset(d->z);
            d->ended = false;
        }
        if (src.size > 0) d->any = true;
        /* With no input left this still runs: output the inflater is holding comes out. */
        proven_size_t used = 0, made = 0;
        bool done = false;
        proven_err_t e = proven_inflate(d->z, src, &used, (proven_mem_mut_t){ .ptr = out.ptr + *produced, .size = out.size - *produced }, &made, &done);
        if (e != PROVEN_OK) return e == PROVEN_ERR_NOMEM ? e : PROVEN_ERR_INVALID_FORMAT;
        if (from_hold) d->held_pos = (proven_u8)(d->held_pos + used);
        else *consumed += used;
        *produced += made;
        if (done) d->ended = true;
        else if (used == 0 && made == 0) return PROVEN_OK;      /* it needs room, or more input */
    }
}

proven_err_t proven_http_decoder_end_(const proven_http_decoder_t_ *d) {
    return (!d->any || d->ended) ? PROVEN_OK : PROVEN_ERR_INVALID_FORMAT;
}

// -----------------------------------------------------------------------------
// Compressing
// -----------------------------------------------------------------------------

bool proven_http_compress_cfg_valid_(const proven_http_compress_cfg_t_ *cfg) {
    return cfg->level >= -1 && cfg->level <= 9 && (cfg->window_bits == 0 || (cfg->window_bits >= 9 && cfg->window_bits <= 15)) &&
           cfg->zstd_level >= 0 && cfg->zstd_level <= 9 && (cfg->zstd_window_log == 0 || (cfg->zstd_window_log >= 10 && cfg->zstd_window_log <= 23));
}

proven_http_coding_t proven_http_compress_choose_(const proven_http_header_t *headers, proven_size_t count, bool zstd) {
    const unsigned g = proven_http_accept_weight(headers, count, PROVEN_HTTP_CODING_GZIP);
    const unsigned z = zstd ? proven_http_accept_weight(headers, count, PROVEN_HTTP_CODING_ZSTD) : 0u;
    /* A tie goes to zstd: the server has offered it, and it costs a fraction of gzip's time. */
    if (z > 0 && z >= g) return PROVEN_HTTP_CODING_ZSTD;
    return g > 0 ? PROVEN_HTTP_CODING_GZIP : PROVEN_HTTP_CODING_IDENTITY;
}

proven_u8str_view_t proven_http_coding_token_(proven_http_coding_t coding) {
    return coding == PROVEN_HTTP_CODING_ZSTD ? PROVEN_LIT("zstd") : PROVEN_LIT("gzip");
}

bool proven_http_compress_applies_(bool accepted, proven_u16 status, const proven_http_header_t *headers, proven_size_t count) {
    if (!accepted) return false;
    if (status == 204 || status == 206 || status == 304) return false;
    proven_u8str_view_t value;
    if (proven_http_header_find(headers, count, PROVEN_LIT("Content-Encoding"), &value)) return false;
    if (proven_http_header_find(headers, count, PROVEN_LIT("Content-Range"), &value)) return false;
    return true;
}

bool proven_http_compress_needs_vary_(const proven_http_header_t *headers, proven_size_t count) {
    return !proven_http_header_has_token(headers, count, PROVEN_LIT("Vary"), PROVEN_LIT("Accept-Encoding")) &&
           !proven_http_header_has_token(headers, count, PROVEN_LIT("Vary"), PROVEN_LIT("*"));
}

struct proven_http_compressor {
    proven_allocator_t alloc;
    proven_deflate_t *gz;
    proven_zstd_encoder_t *zs;
};

static proven_zstd_encoder_options_t hc_zstd_options(proven_allocator_t alloc, const proven_http_compress_cfg_t_ *cfg) {
    return (proven_zstd_encoder_options_t){ .alloc = alloc, .level = cfg->zstd_level ? cfg->zstd_level : 1, .window_log = cfg->zstd_window_log ? cfg->zstd_window_log : 17 };
}

proven_err_t proven_http_compressor_create_(proven_allocator_t alloc, proven_http_coding_t coding, const proven_http_compress_cfg_t_ *cfg,
                                            proven_http_compressor_t_ **out) {
    *out = (void *)0;
    proven_result_mem_mut_t m = alloc.alloc_fn(alloc.ctx, sizeof(proven_http_compressor_t_), 16);
    if (m.err != PROVEN_OK) return PROVEN_ERR_NOMEM;
    proven_http_compressor_t_ *c = (proven_http_compressor_t_ *)(void *)m.value.ptr;
    *c = (proven_http_compressor_t_){ .alloc = alloc };
    proven_err_t e;
    if (coding == PROVEN_HTTP_CODING_ZSTD) {
        const proven_zstd_encoder_options_t o = hc_zstd_options(alloc, cfg);
        e = proven_zstd_encoder_create(&o, &c->zs);
    } else {
        const proven_deflate_options_t o = { .alloc = alloc, .format = PROVEN_DEFLATE_GZIP, .level = cfg->level, .window_bits = cfg->window_bits };
        e = proven_deflate_create(&o, &c->gz);
    }
    if (e != PROVEN_OK) {
        alloc.free_fn(alloc.ctx, c);
        return e;
    }
    *out = c;
    return PROVEN_OK;
}

void proven_http_compressor_destroy_(proven_http_compressor_t_ *c) {
    if (!c) return;
    proven_deflate_destroy(c->gz);
    proven_zstd_encoder_destroy(c->zs);
    c->alloc.free_fn(c->alloc.ctx, c);
}

proven_err_t proven_http_compressor_step_(proven_http_compressor_t_ *c, proven_mem_view_t in, proven_size_t *consumed,
                                          proven_mem_mut_t out, proven_size_t *produced, proven_deflate_flush_t flush, bool *done) {
    if (c->gz) return proven_deflate(c->gz, in, consumed, out, produced, flush, done);
    const proven_zstd_flush_t f = flush == PROVEN_DEFLATE_FLUSH_FINISH ? PROVEN_ZSTD_FLUSH_FINISH
                                : flush == PROVEN_DEFLATE_FLUSH_SYNC ? PROVEN_ZSTD_FLUSH_BLOCK : PROVEN_ZSTD_FLUSH_NONE;
    return proven_zstd_encode(c->zs, in, consumed, out, produced, f, done);
}

proven_err_t proven_http_compress_all_(proven_allocator_t alloc, proven_http_coding_t coding, const proven_http_compress_cfg_t_ *cfg, proven_mem_view_t body,
                                       proven_byte_t **out, proven_size_t *out_cap, proven_size_t *out_size) {
    *out = (void *)0;
    *out_cap = 0;
    *out_size = 0;
    if (body.size < PROVEN_HTTP_CODING_MIN_BODY) return PROVEN_OK;
    /* The result must be smaller than the body to be worth sending, so that is all the room
     * it gets. */
    proven_size_t cap = body.size - 1;
    proven_result_mem_mut_t m = alloc.alloc_fn(alloc.ctx, cap, 1);
    if (m.err != PROVEN_OK) return m.err;
    proven_size_t written = 0;
    proven_err_t e;
    if (coding == PROVEN_HTTP_CODING_ZSTD) {
        /* Compressed in place: the encoder sizes its tables and its window to the body. */
        const proven_zstd_encoder_options_t o = hc_zstd_options(alloc, cfg);
        e = proven_zstd_encode_all(&o, body, (proven_mem_mut_t){ .ptr = m.value.ptr, .size = cap }, &written);
    } else {
        /* No window larger than the body can use: the compressor's state follows the window. */
        int limit = cfg->window_bits == 0 ? 15 : cfg->window_bits;
        int bits = 9;
        while (bits < limit && ((proven_size_t)1 << bits) < body.size) bits++;
        proven_deflate_options_t o = { .alloc = alloc, .format = PROVEN_DEFLATE_GZIP, .level = cfg->level, .window_bits = bits };
        e = proven_deflate_all(&o, body, (proven_mem_mut_t){ .ptr = m.value.ptr, .size = cap }, &written);
    }
    if (e != PROVEN_OK) {
        alloc.free_fn(alloc.ctx, m.value.ptr);
        return e == PROVEN_ERR_OUT_OF_BOUNDS ? PROVEN_OK : e;      /* it did not shrink */
    }
    *out = m.value.ptr;
    *out_cap = cap;
    *out_size = written;
    return PROVEN_OK;
}

// ---------------------------------------------------------------------------------------
// Trailers kept for the event-driven drivers
// ---------------------------------------------------------------------------------------

void proven_http_trailers_free_(proven_allocator_t alloc, proven_http_trailers_t_ *t) {
    if (t->fields) alloc.free_fn(alloc.ctx, t->fields);
    *t = (proven_http_trailers_t_){ 0 };
}

proven_err_t proven_http_trailers_keep_(proven_allocator_t alloc, proven_http_trailers_t_ *t, const proven_http_header_t *fields, proven_size_t count, bool drop, bool lower) {
    proven_http_trailers_free_(alloc, t);
    proven_size_t kept = 0, bytes = 0;
    for (proven_size_t i = 0; i < count; ++i) {
        if (drop && !proven_http_trailer_allowed(fields[i].name)) continue;
        kept++;
        bytes += fields[i].name.size + fields[i].value.size;
        if (bytes > PROVEN_HTTP_MAX_TRAILER_BYTES) return PROVEN_ERR_OUT_OF_BOUNDS;
    }
    if (kept == 0) return PROVEN_OK;
    const proven_size_t size = kept * sizeof(proven_http_header_t) + bytes;
    proven_result_mem_mut_t m = alloc.alloc_fn(alloc.ctx, size, _Alignof(proven_http_header_t));
    if (m.err != PROVEN_OK || !m.value.ptr) return PROVEN_ERR_NOMEM;
    proven_http_header_t *out = (proven_http_header_t *)(void *)m.value.ptr;
    proven_byte_t *at = m.value.ptr + kept * sizeof(proven_http_header_t);
    proven_size_t n = 0;
    for (proven_size_t i = 0; i < count; ++i) {
        if (drop && !proven_http_trailer_allowed(fields[i].name)) continue;
        for (proven_size_t k = 0; k < fields[i].name.size; ++k) {
            const proven_byte_t c = fields[i].name.ptr[k];
            at[k] = lower ? proven_ascii_lower(c) : c;
        }
        out[n].name = (proven_u8str_view_t){ .ptr = at, .size = fields[i].name.size };
        at += fields[i].name.size;
        for (proven_size_t k = 0; k < fields[i].value.size; ++k) at[k] = fields[i].value.ptr[k];
        out[n].value = (proven_u8str_view_t){ .ptr = at, .size = fields[i].value.size };
        at += fields[i].value.size;
        n++;
    }
    *t = (proven_http_trailers_t_){ .fields = out, .count = n, .size = size };
    return PROVEN_OK;
}

proven_err_t proven_http_trailers_give_(const proven_http_trailers_t_ *t, proven_http_header_t *fields, proven_size_t cap, proven_size_t *count) {
    *count = 0;
    if (t->count > cap) return PROVEN_ERR_OUT_OF_BOUNDS;
    for (proven_size_t i = 0; i < t->count; ++i) fields[i] = t->fields[i];
    *count = t->count;
    return PROVEN_OK;
}
