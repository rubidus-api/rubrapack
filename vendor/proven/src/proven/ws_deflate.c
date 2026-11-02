#include "proven_internal_ws.h"

/*
 * permessage-deflate (RFC 7692) for the WebSocket drivers: one message in, one message out.
 * Between messages nothing is kept, unless a direction takes over: then its compressor or
 * decompressor is the connection's.
 *
 * On the wire a compressed message is raw DEFLATE ended by a sync flush - an empty stored
 * block, whose last four bytes are always 00 00 ff ff - with those four bytes left off. The
 * receiver puts them back and inflates.
 */

static const proven_byte_t WS_TAIL[4] = { 0x00, 0x00, 0xff, 0xff };

// -----------------------------------------------------------------------------
// Receiving
// -----------------------------------------------------------------------------

proven_err_t proven_ws_inflate_begin_(proven_ws_inflate_t_ *m, proven_allocator_t alloc, bool text, proven_inflate_t **kept) {
    *m = (proven_ws_inflate_t_){ .text = text, .kept = kept != (void *)0 };
    if (!kept) return proven_inflate_create(alloc, PROVEN_DEFLATE_RAW, &m->z);
    if (!*kept) {
        proven_err_t e = proven_inflate_create(alloc, PROVEN_DEFLATE_RAW, kept);
        if (e != PROVEN_OK) return e;
    }
    m->z = *kept;
    return PROVEN_OK;
}

void proven_ws_inflate_end_(proven_ws_inflate_t_ *m) {
    if (m && m->z) {
        if (!m->kept) proven_inflate_destroy(m->z);
        m->z = (void *)0;
    }
}

proven_err_t proven_ws_inflate_step_(proven_ws_inflate_t_ *m, proven_mem_view_t in, proven_size_t *consumed, bool last,
                                     proven_mem_mut_t out, proven_size_t *produced, proven_u64 max_bytes, bool *done) {
    *consumed = 0;
    *produced = 0;
    *done = false;
    for (;;) {
        /* The message's own bytes first, then - once there are no more - the four the sender
         * left off. */
        bool from_tail = *consumed == in.size && last && m->tail < 4;
        proven_mem_view_t src = from_tail ? (proven_mem_view_t){ .ptr = WS_TAIL + m->tail, .size = (proven_size_t)(4 - m->tail) }
                                          : (proven_mem_view_t){ .ptr = in.ptr + *consumed, .size = in.size - *consumed };
        proven_size_t used = 0, made = 0;
        bool ended = false;
        proven_byte_t *at = out.ptr + *produced;
        proven_err_t e = proven_inflate(m->z, src, &used, (proven_mem_mut_t){ .ptr = at, .size = out.size - *produced }, &made, &ended);
        if (e != PROVEN_OK) return e == PROVEN_ERR_NOMEM ? e : PROVEN_ERR_INVALID_FORMAT;
        if (from_tail) m->tail = (proven_u8)(m->tail + used);
        else *consumed += used;
        *produced += made;
        m->total += made;
        if (max_bytes != 0 && m->total > max_bytes) return PROVEN_ERR_OUT_OF_BOUNDS;
        if (m->text && made > 0) {
            e = proven_ws_utf8_feed_(m->held, &m->held_len, at, made);
            if (e != PROVEN_OK) return e;
        }
        /* A sender may close its last block instead of flushing (RFC 7692, 7.2.3.4). What
         * follows is then read as the start of a stream - one that, under takeover, may still
         * refer back to everything before it. */
        if (ended) {
            if (m->kept) proven_inflate_restart(m->z);
            else proven_inflate_reset(m->z);
            continue;
        }
        if (used == 0 && made == 0) {
            /* It wants input, or room. With every byte given and room to spare, that is the
             * end of the message. */
            if (last && *consumed == in.size && m->tail == 4 && *produced < out.size) {
                if (m->text && m->held_len != 0) return PROVEN_ERR_INVALID_ENCODING;
                *done = true;
            }
            return PROVEN_OK;
        }
        if (*produced == out.size) return PROVEN_OK;
    }
}

// -----------------------------------------------------------------------------
// Sending
// -----------------------------------------------------------------------------

proven_err_t proven_ws_deflate_begin_(proven_allocator_t alloc, int level, int window_bits, proven_size_t size_hint, proven_deflate_t **out) {
    int limit = window_bits == 0 ? 15 : window_bits;
    int bits = limit;
    if (size_hint != 0) {
        /* No window larger than the message can use: the compressor's state follows it. */
        bits = 9;
        while (bits < limit && ((proven_size_t)1 << bits) < size_hint) bits++;
    }
    proven_deflate_options_t o = { .alloc = alloc, .format = PROVEN_DEFLATE_RAW, .level = level, .window_bits = bits };
    return proven_deflate_create(&o, out);
}

proven_err_t proven_ws_deflate_step_(proven_deflate_t *z, proven_mem_view_t in, proven_size_t *consumed, bool flush, bool last,
                                     proven_mem_mut_t out, proven_size_t *produced, proven_byte_t tail[4], proven_u8 *tail_len, bool *done) {
    *consumed = 0;
    *produced = 0;
    *done = false;
    if (out.size < 16) return PROVEN_ERR_INVALID_ARG;
    /* The last four bytes produced are always held back, because at the end of the message
     * they are the four that are not sent. So the compressor writes after the place where
     * the bytes held from the call before will go. */
    proven_size_t held = *tail_len;
    proven_size_t fresh = 0;
    bool complete = false;
    while (held + fresh < out.size - 4) {
        proven_size_t used = 0, made = 0;
        bool finished = false;
        proven_err_t e = proven_deflate(z, (proven_mem_view_t){ .ptr = in.ptr + *consumed, .size = in.size - *consumed }, &used,
                                        (proven_mem_mut_t){ .ptr = out.ptr + held + fresh, .size = out.size - 4 - held - fresh }, &made,
                                        flush ? PROVEN_DEFLATE_FLUSH_SYNC : PROVEN_DEFLATE_FLUSH_NONE, &finished);
        if (e != PROVEN_OK) return e;
        *consumed += used;
        fresh += made;
        if (made == 0 && *consumed == in.size) { complete = true; break; }     /* it has nothing more to give */
        if (made == 0 && used == 0) break;
    }
    for (proven_size_t i = 0; i < held; ++i) out.ptr[i] = tail[i];
    proven_size_t all = held + fresh;
    if (!complete) {
        /* More will follow: keep the last four back. */
        proven_size_t keep = all < 4 ? all : 4;
        for (proven_size_t i = 0; i < keep; ++i) tail[i] = out.ptr[all - keep + i];
        *tail_len = (proven_u8)keep;
        *produced = all - keep;
        return PROVEN_OK;
    }
    *tail_len = 0;
    *done = true;
    if (flush && last) {
        /* The end of the message: the flush's four bytes stay here. When this flush had
         * nothing to say - an empty last part after a part that was flushed - the message
         * ends with the one byte that begins an empty block, which the receiver's four
         * complete. */
        if (all >= 4) *produced = all - 4;
        else { out.ptr[0] = 0x00; *produced = 1; }
        return PROVEN_OK;
    }
    *produced = all;
    return PROVEN_OK;
}
