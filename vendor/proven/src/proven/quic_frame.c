#include "proven_internal_quic.h"
#include "../../platform/proven_sys_mem.h"

#ifndef PROVEN_NO_HTTP3

/* QUIC version 1's packet headers and frames (RFC 9000 sections 12, 17 and 19): reading and
 * writing, nothing else. Pure computation: no I/O, no allocation, no state.
 *
 * A header is read from a datagram anyone may have sent, before anything is authenticated: it
 * is only located here, never trusted. A frame is read from a packet that has opened. */

#define STREAM_LIMIT ((((proven_u64)1) << 62) - 1)     /* the largest offset a stream may reach */
#define MAX_STREAM_COUNT (((proven_u64)1) << 60)

/* ---- Headers ---- */

static bool rd_cid(proven_quic_rd_t *r, proven_quic_cid_t *cid) {
    proven_u64 len = proven_quic_rd_uint(r, 1);
    if (!r->ok || len > PROVEN_QUIC_MAX_CID) return false;
    proven_quic_rd_t b = proven_quic_rd_take(r, (proven_size_t)len);
    if (!r->ok) return false;
    cid->len = (proven_byte_t)len;
    for (proven_size_t i = 0; i < len; ++i) cid->id[i] = b.p[i];
    return true;
}

bool proven_quic_hdr_parse(proven_mem_view_t d, proven_size_t short_dcid_len, proven_quic_hdr_t *h) {
    *h = (proven_quic_hdr_t){ 0 };
    proven_quic_rd_t r = { d.ptr, d.size, true };
    const proven_u64 first = proven_quic_rd_uint(&r, 1);
    if (!r.ok) return false;
    if (!(first & 0x80)) {
        /* A short header: the fixed bit, then this side's connection ID. */
        if (!(first & 0x40) || short_dcid_len > PROVEN_QUIC_MAX_CID) return false;
        proven_quic_rd_t b = proven_quic_rd_take(&r, short_dcid_len);
        if (!r.ok) return false;
        h->type = PROVEN_QUIC_PKT_SHORT;
        h->version = PROVEN_QUIC_VERSION_1;
        h->dcid.len = (proven_byte_t)short_dcid_len;
        for (proven_size_t i = 0; i < short_dcid_len; ++i) h->dcid.id[i] = b.p[i];
        h->pn_offset = 1 + short_dcid_len;
        h->len = d.size;
        return true;
    }
    h->version = (proven_u32)proven_quic_rd_uint(&r, 4);
    if (!r.ok || !rd_cid(&r, &h->dcid) || !rd_cid(&r, &h->scid)) return false;
    if (h->version == 0) {
        /* Version negotiation: what follows is a list of versions, four bytes each. */
        if (r.n == 0 || r.n % 4 != 0) return false;
        h->type = PROVEN_QUIC_PKT_VN;
        h->token = (proven_mem_view_t){ .ptr = r.p, .size = r.n };
        h->len = d.size;
        return true;
    }
    if (h->version != PROVEN_QUIC_VERSION_1 && h->version != PROVEN_QUIC_VERSION_2) {
        h->type = PROVEN_QUIC_PKT_OTHER_VERSION;
        h->len = d.size;
        return true;
    }
    if (!(first & 0x40)) return false;
    /* Version 2 moves every type up by one (RFC 9369 section 3.2): read as version 1's. */
    const unsigned bits = (unsigned)(first >> 4) & 3;
    const unsigned type = h->version == PROVEN_QUIC_VERSION_2 ? (bits + 3) & 3 : bits;
    if (type == 3) {
        /* Retry: a token, then the 16-byte integrity tag. A Retry with no token is discarded. */
        if (r.n <= 16) return false;
        h->type = PROVEN_QUIC_PKT_RETRY;
        h->token = (proven_mem_view_t){ .ptr = r.p, .size = r.n - 16 };
        h->len = d.size;
        return true;
    }
    h->type = type == 0 ? PROVEN_QUIC_PKT_INITIAL : type == 1 ? PROVEN_QUIC_PKT_0RTT : PROVEN_QUIC_PKT_HANDSHAKE;
    if (type == 0) {
        const proven_u64 tlen = proven_quic_rd_varint(&r);
        if (!r.ok || tlen > r.n) return false;
        h->token = (proven_mem_view_t){ .ptr = r.p, .size = (proven_size_t)tlen };
        (void)proven_quic_rd_take(&r, (proven_size_t)tlen);
    }
    const proven_u64 length = proven_quic_rd_varint(&r);
    if (!r.ok || length > r.n) return false;
    h->pn_offset = d.size - r.n;
    h->len = h->pn_offset + (proven_size_t)length;
    return true;
}

proven_size_t proven_quic_vn_build(const proven_quic_cid_t *dcid, const proven_quic_cid_t *scid, proven_byte_t random_bits, proven_mem_mut_t out) {
    proven_quic_wr_t w = { out.ptr, 0, out.size, true };
    proven_quic_wr_uint(&w, 0x80u | (random_bits & 0x7fu), 1);
    proven_quic_wr_uint(&w, 0, 4);
    proven_quic_wr_uint(&w, dcid->len, 1); proven_quic_wr_bytes(&w, dcid->id, dcid->len);
    proven_quic_wr_uint(&w, scid->len, 1); proven_quic_wr_bytes(&w, scid->id, scid->len);
    proven_quic_wr_uint(&w, PROVEN_QUIC_VERSION_1, 4);
    proven_quic_wr_uint(&w, PROVEN_QUIC_VERSION_2, 4);
    return w.ok ? w.n : 0;
}

/* Version 1's type bits (Initial 0, 0-RTT 1, Handshake 2, Retry 3) in `version`'s place. */
static unsigned type_bits(proven_u32 version, unsigned v1_bits) { return version == PROVEN_QUIC_VERSION_2 ? (v1_bits + 1) & 3 : v1_bits; }

proven_size_t proven_quic_retry_build(const proven_quic_cid_t *odcid, const proven_quic_cid_t *dcid, const proven_quic_cid_t *scid,
                                      proven_mem_view_t token, proven_byte_t random_bits, proven_mem_mut_t out) {
    return proven_quic_retry_build_v(PROVEN_QUIC_VERSION_1, odcid, dcid, scid, token, random_bits, out);
}

proven_size_t proven_quic_retry_build_v(proven_u32 version, const proven_quic_cid_t *odcid, const proven_quic_cid_t *dcid, const proven_quic_cid_t *scid,
                                        proven_mem_view_t token, proven_byte_t random_bits, proven_mem_mut_t out) {
    proven_quic_wr_t w = { out.ptr, 0, out.size, true };
    proven_quic_wr_uint(&w, 0xc0u | (type_bits(version, 3) << 4) | (random_bits & 0x0fu), 1);
    proven_quic_wr_uint(&w, version, 4);
    proven_quic_wr_uint(&w, dcid->len, 1); proven_quic_wr_bytes(&w, dcid->id, dcid->len);
    proven_quic_wr_uint(&w, scid->len, 1); proven_quic_wr_bytes(&w, scid->id, scid->len);
    proven_quic_wr_bytes(&w, token.ptr, token.size);
    if (!w.ok || w.cap - w.n < 16) return 0;
    proven_quic_retry_tag((proven_mem_view_t){ .ptr = odcid->id, .size = odcid->len }, (proven_mem_view_t){ .ptr = out.ptr, .size = w.n }, out.ptr + w.n);
    return w.n + 16;
}

proven_size_t proven_quic_pn_len(proven_u64 pn, proven_u64 largest_acked_plus_one) {
    /* RFC 9000 appendix A.2: room for twice the packets not yet acknowledged. */
    const proven_u64 unacked = pn + 1 - largest_acked_plus_one;
    for (proven_size_t len = 1; len < 4; ++len) if (unacked <= ((proven_u64)1 << (8 * len - 1))) return len;
    return 4;
}

proven_size_t proven_quic_long_header_put(proven_quic_pkt_type_t type, const proven_quic_cid_t *dcid, const proven_quic_cid_t *scid,
                                          proven_mem_view_t token, proven_u64 pn, proven_size_t pn_len, proven_size_t payload_len,
                                          proven_mem_mut_t out) {
    return proven_quic_long_header_put_v(PROVEN_QUIC_VERSION_1, type, dcid, scid, token, pn, pn_len, payload_len, out);
}

proven_size_t proven_quic_long_header_put_v(proven_u32 version, proven_quic_pkt_type_t type, const proven_quic_cid_t *dcid, const proven_quic_cid_t *scid,
                                            proven_mem_view_t token, proven_u64 pn, proven_size_t pn_len, proven_size_t payload_len,
                                            proven_mem_mut_t out) {
    const unsigned bits = type == PROVEN_QUIC_PKT_INITIAL ? 0 : type == PROVEN_QUIC_PKT_0RTT ? 1 : type == PROVEN_QUIC_PKT_HANDSHAKE ? 2 : 4;
    const proven_size_t length = pn_len + payload_len + 16;
    if (bits == 4 || pn_len < 1 || pn_len > 4 || length >= 16384) return 0;
    proven_quic_wr_t w = { out.ptr, 0, out.size, true };
    proven_quic_wr_uint(&w, 0xc0u | (type_bits(version, bits) << 4) | (unsigned)(pn_len - 1), 1);
    proven_quic_wr_uint(&w, version, 4);
    proven_quic_wr_uint(&w, dcid->len, 1); proven_quic_wr_bytes(&w, dcid->id, dcid->len);
    proven_quic_wr_uint(&w, scid->len, 1); proven_quic_wr_bytes(&w, scid->id, scid->len);
    if (type == PROVEN_QUIC_PKT_INITIAL) { proven_quic_wr_varint(&w, token.size); proven_quic_wr_bytes(&w, token.ptr, token.size); }
    proven_quic_wr_uint(&w, 0x4000u | length, 2);
    proven_quic_wr_uint(&w, pn, pn_len);
    return w.ok ? w.n : 0;
}

proven_size_t proven_quic_short_header_put(const proven_quic_cid_t *dcid, bool key_phase, proven_u64 pn, proven_size_t pn_len, proven_mem_mut_t out) {
    if (pn_len < 1 || pn_len > 4) return 0;
    proven_quic_wr_t w = { out.ptr, 0, out.size, true };
    proven_quic_wr_uint(&w, 0x40u | (key_phase ? 0x04u : 0u) | (unsigned)(pn_len - 1), 1);
    proven_quic_wr_bytes(&w, dcid->id, dcid->len);
    proven_quic_wr_uint(&w, pn, pn_len);
    return w.ok ? w.n : 0;
}

/* ---- The writer ---- */

void proven_quic_wr_bytes(proven_quic_wr_t *w, const proven_byte_t *p, proven_size_t n) {
    if (!w->ok || w->cap - w->n < n) { w->ok = false; return; }
    proven_sys_mem_copy(w->p + w->n, p, n);
    w->n += n;
}

void proven_quic_wr_uint(proven_quic_wr_t *w, proven_u64 v, proven_size_t bytes) {
    if (!w->ok || w->cap - w->n < bytes) { w->ok = false; return; }
    for (proven_size_t i = 0; i < bytes; ++i) w->p[w->n + i] = (proven_byte_t)(v >> (8 * (bytes - 1 - i)));
    w->n += bytes;
}

void proven_quic_wr_varint(proven_quic_wr_t *w, proven_u64 v) {
    proven_byte_t b[8];
    const proven_size_t n = proven_quic_varint_put(v, b);
    if (n == 0) { w->ok = false; return; }
    proven_quic_wr_bytes(w, b, n);
}

/* ---- Frames ---- */

bool proven_quic_frame_in_handshake(proven_u64 type) {
    return type == PROVEN_QUIC_FT_PADDING || type == PROVEN_QUIC_FT_PING || type == PROVEN_QUIC_FT_ACK || type == PROVEN_QUIC_FT_ACK_ECN
        || type == PROVEN_QUIC_FT_CRYPTO || type == PROVEN_QUIC_FT_CONNECTION_CLOSE;
}

bool proven_quic_frame_ack_eliciting(proven_u64 type) {
    return type != PROVEN_QUIC_FT_PADDING && type != PROVEN_QUIC_FT_ACK && type != PROVEN_QUIC_FT_ACK_ECN
        && type != PROVEN_QUIC_FT_CONNECTION_CLOSE && type != PROVEN_QUIC_FT_CONNECTION_CLOSE_APP;
}

bool proven_quic_frame_probing(proven_u64 type) {
    return type == PROVEN_QUIC_FT_PADDING || type == PROVEN_QUIC_FT_PATH_CHALLENGE || type == PROVEN_QUIC_FT_PATH_RESPONSE
        || type == PROVEN_QUIC_FT_NEW_CONNECTION_ID;
}

static proven_mem_view_t rd_view(proven_quic_rd_t *r, proven_u64 n) {
    if (!r->ok || n > r->n) { r->ok = false; return (proven_mem_view_t){ .ptr = r->p, .size = 0 }; }
    proven_quic_rd_t b = proven_quic_rd_take(r, (proven_size_t)n);
    return (proven_mem_view_t){ .ptr = b.p, .size = b.n };
}

proven_u64 proven_quic_frame_read(proven_quic_rd_t *r, proven_quic_frame_t *f) {
    *f = (proven_quic_frame_t){ 0 };
    const proven_size_t before = r->n;
    const proven_u64 type = proven_quic_rd_varint(r);
    if (!r->ok) return PROVEN_QUIC_FRAME_ENCODING_ERROR;
    if (before - r->n != proven_quic_varint_len(type)) return PROVEN_QUIC_PROTOCOL_VIOLATION;
    f->type = type >= 0x08 && type <= 0x0f ? PROVEN_QUIC_FT_STREAM : type;
    switch (type) {
    case PROVEN_QUIC_FT_PADDING: case PROVEN_QUIC_FT_PING: case PROVEN_QUIC_FT_HANDSHAKE_DONE:
        break;
    case PROVEN_QUIC_FT_ACK: case PROVEN_QUIC_FT_ACK_ECN: {
        f->largest = proven_quic_rd_varint(r);
        f->ack_delay = proven_quic_rd_varint(r);
        f->range_count = proven_quic_rd_varint(r);
        f->first_range = proven_quic_rd_varint(r);
        if (!r->ok || f->first_range > f->largest) return PROVEN_QUIC_FRAME_ENCODING_ERROR;
        /* Every range must stay at or above packet number 0: checked here, once. */
        const proven_byte_t *start = r->p;
        proven_u64 lo = f->largest - f->first_range;
        for (proven_u64 i = 0; i < f->range_count; ++i) {
            const proven_u64 gap = proven_quic_rd_varint(r), len = proven_quic_rd_varint(r);
            if (!r->ok || lo < 2 || gap > lo - 2) return PROVEN_QUIC_FRAME_ENCODING_ERROR;
            const proven_u64 hi = lo - gap - 2;
            if (len > hi) return PROVEN_QUIC_FRAME_ENCODING_ERROR;
            lo = hi - len;
        }
        f->ranges = (proven_mem_view_t){ .ptr = start, .size = (proven_size_t)(r->p - start) };
        if (type == PROVEN_QUIC_FT_ACK_ECN) for (int i = 0; i < 3; ++i) f->ecn[i] = proven_quic_rd_varint(r);
        break;
    }
    case PROVEN_QUIC_FT_RESET_STREAM:
        f->id = proven_quic_rd_varint(r); f->value = proven_quic_rd_varint(r); f->final_size = proven_quic_rd_varint(r);
        break;
    case PROVEN_QUIC_FT_STOP_SENDING:
        f->id = proven_quic_rd_varint(r); f->value = proven_quic_rd_varint(r);
        break;
    case PROVEN_QUIC_FT_CRYPTO: {
        f->offset = proven_quic_rd_varint(r);
        const proven_u64 len = proven_quic_rd_varint(r);
        f->data = rd_view(r, len);
        if (r->ok && len > STREAM_LIMIT - f->offset) return PROVEN_QUIC_FRAME_ENCODING_ERROR;
        break;
    }
    case PROVEN_QUIC_FT_NEW_TOKEN: {
        const proven_u64 len = proven_quic_rd_varint(r);
        f->data = rd_view(r, len);
        if (r->ok && len == 0) return PROVEN_QUIC_FRAME_ENCODING_ERROR;
        break;
    }
    case PROVEN_QUIC_FT_MAX_DATA: case PROVEN_QUIC_FT_DATA_BLOCKED:
        f->value = proven_quic_rd_varint(r);
        break;
    case PROVEN_QUIC_FT_MAX_STREAM_DATA: case PROVEN_QUIC_FT_STREAM_DATA_BLOCKED:
        f->id = proven_quic_rd_varint(r); f->value = proven_quic_rd_varint(r);
        break;
    case PROVEN_QUIC_FT_MAX_STREAMS_BIDI: case PROVEN_QUIC_FT_MAX_STREAMS_UNI:
    case PROVEN_QUIC_FT_STREAMS_BLOCKED_BIDI: case PROVEN_QUIC_FT_STREAMS_BLOCKED_UNI:
        f->value = proven_quic_rd_varint(r);
        if (r->ok && f->value > MAX_STREAM_COUNT) return PROVEN_QUIC_FRAME_ENCODING_ERROR;
        break;
    case PROVEN_QUIC_FT_NEW_CONNECTION_ID: {
        f->id = proven_quic_rd_varint(r);
        f->value = proven_quic_rd_varint(r);
        const proven_u64 len = proven_quic_rd_uint(r, 1);
        if (!r->ok || len < 1 || len > PROVEN_QUIC_MAX_CID || f->value > f->id) return PROVEN_QUIC_FRAME_ENCODING_ERROR;
        proven_mem_view_t cid = rd_view(r, len), token = rd_view(r, PROVEN_QUIC_RESET_TOKEN);
        if (!r->ok) return PROVEN_QUIC_FRAME_ENCODING_ERROR;
        f->cid.len = (proven_byte_t)len;
        for (proven_size_t i = 0; i < len; ++i) f->cid.id[i] = cid.ptr[i];
        for (proven_size_t i = 0; i < PROVEN_QUIC_RESET_TOKEN; ++i) f->reset_token[i] = token.ptr[i];
        break;
    }
    case PROVEN_QUIC_FT_RETIRE_CONNECTION_ID:
        f->id = proven_quic_rd_varint(r);
        break;
    case PROVEN_QUIC_FT_PATH_CHALLENGE: case PROVEN_QUIC_FT_PATH_RESPONSE:
        f->data = rd_view(r, 8);
        break;
    case PROVEN_QUIC_FT_CONNECTION_CLOSE: case PROVEN_QUIC_FT_CONNECTION_CLOSE_APP: {
        f->value = proven_quic_rd_varint(r);
        if (type == PROVEN_QUIC_FT_CONNECTION_CLOSE) f->frame_type = proven_quic_rd_varint(r);
        const proven_u64 len = proven_quic_rd_varint(r);
        f->data = rd_view(r, len);
        break;
    }
    default:
        if (type >= 0x08 && type <= 0x0f) {
            f->id = proven_quic_rd_varint(r);
            if (type & 0x04) f->offset = proven_quic_rd_varint(r);
            const proven_u64 len = type & 0x02 ? proven_quic_rd_varint(r) : r->n;
            f->data = rd_view(r, len);
            f->fin = (type & 0x01) != 0;
            if (r->ok && len > STREAM_LIMIT - f->offset) return PROVEN_QUIC_FRAME_ENCODING_ERROR;
            break;
        }
        return PROVEN_QUIC_FRAME_ENCODING_ERROR;
    }
    return r->ok ? 0 : PROVEN_QUIC_FRAME_ENCODING_ERROR;
}

proven_size_t proven_quic_stream_overhead(proven_u64 id, proven_u64 offset, proven_size_t len) {
    return 1 + proven_quic_varint_len(id) + (offset ? proven_quic_varint_len(offset) : 0) + proven_quic_varint_len(len);
}

bool proven_quic_frame_put(proven_quic_wr_t *w, const proven_quic_frame_t *f) {
    const proven_size_t at = w->n;
    switch (f->type) {
    case PROVEN_QUIC_FT_PADDING: case PROVEN_QUIC_FT_PING: case PROVEN_QUIC_FT_HANDSHAKE_DONE:
        proven_quic_wr_varint(w, f->type);
        break;
    case PROVEN_QUIC_FT_RESET_STREAM:
        proven_quic_wr_varint(w, f->type); proven_quic_wr_varint(w, f->id); proven_quic_wr_varint(w, f->value); proven_quic_wr_varint(w, f->final_size);
        break;
    case PROVEN_QUIC_FT_STOP_SENDING: case PROVEN_QUIC_FT_MAX_STREAM_DATA: case PROVEN_QUIC_FT_STREAM_DATA_BLOCKED:
        proven_quic_wr_varint(w, f->type); proven_quic_wr_varint(w, f->id); proven_quic_wr_varint(w, f->value);
        break;
    case PROVEN_QUIC_FT_CRYPTO:
        proven_quic_wr_varint(w, f->type); proven_quic_wr_varint(w, f->offset);
        proven_quic_wr_varint(w, f->data.size); proven_quic_wr_bytes(w, f->data.ptr, f->data.size);
        break;
    case PROVEN_QUIC_FT_NEW_TOKEN:
        proven_quic_wr_varint(w, f->type); proven_quic_wr_varint(w, f->data.size); proven_quic_wr_bytes(w, f->data.ptr, f->data.size);
        break;
    case PROVEN_QUIC_FT_STREAM:
        proven_quic_wr_varint(w, 0x08u | (f->offset ? 0x04u : 0u) | 0x02u | (f->fin ? 0x01u : 0u));
        proven_quic_wr_varint(w, f->id);
        if (f->offset) proven_quic_wr_varint(w, f->offset);
        proven_quic_wr_varint(w, f->data.size); proven_quic_wr_bytes(w, f->data.ptr, f->data.size);
        break;
    case PROVEN_QUIC_FT_MAX_DATA: case PROVEN_QUIC_FT_DATA_BLOCKED:
    case PROVEN_QUIC_FT_MAX_STREAMS_BIDI: case PROVEN_QUIC_FT_MAX_STREAMS_UNI:
    case PROVEN_QUIC_FT_STREAMS_BLOCKED_BIDI: case PROVEN_QUIC_FT_STREAMS_BLOCKED_UNI:
        proven_quic_wr_varint(w, f->type); proven_quic_wr_varint(w, f->value);
        break;
    case PROVEN_QUIC_FT_NEW_CONNECTION_ID:
        proven_quic_wr_varint(w, f->type); proven_quic_wr_varint(w, f->id); proven_quic_wr_varint(w, f->value);
        proven_quic_wr_uint(w, f->cid.len, 1); proven_quic_wr_bytes(w, f->cid.id, f->cid.len);
        proven_quic_wr_bytes(w, f->reset_token, PROVEN_QUIC_RESET_TOKEN);
        break;
    case PROVEN_QUIC_FT_RETIRE_CONNECTION_ID:
        proven_quic_wr_varint(w, f->type); proven_quic_wr_varint(w, f->id);
        break;
    case PROVEN_QUIC_FT_PATH_CHALLENGE: case PROVEN_QUIC_FT_PATH_RESPONSE:
        proven_quic_wr_varint(w, f->type);
        if (f->data.size != 8) w->ok = false; else proven_quic_wr_bytes(w, f->data.ptr, 8);
        break;
    case PROVEN_QUIC_FT_CONNECTION_CLOSE: case PROVEN_QUIC_FT_CONNECTION_CLOSE_APP:
        proven_quic_wr_varint(w, f->type); proven_quic_wr_varint(w, f->value);
        if (f->type == PROVEN_QUIC_FT_CONNECTION_CLOSE) proven_quic_wr_varint(w, f->frame_type);
        proven_quic_wr_varint(w, f->data.size); proven_quic_wr_bytes(w, f->data.ptr, f->data.size);
        break;
    default:
        w->ok = false;
    }
    if (!w->ok) { w->n = at; w->ok = true; return false; }
    return true;
}

proven_quic_ack_iter_t proven_quic_ack_iter(const proven_quic_frame_t *f) {
    return (proven_quic_ack_iter_t){ .r = { f->ranges.ptr, f->ranges.size, true }, .hi = f->largest, .lo = f->largest - f->first_range,
                                     .left = f->range_count, .first = true };
}

bool proven_quic_ack_next(proven_quic_ack_iter_t *it, proven_u64 *lo, proven_u64 *hi) {
    if (it->first) { it->first = false; *lo = it->lo; *hi = it->hi; return true; }
    if (it->left == 0) return false;
    const proven_u64 gap = proven_quic_rd_varint(&it->r), len = proven_quic_rd_varint(&it->r);
    if (!it->r.ok) return false;                    /* the frame was checked when read: not reached */
    it->hi = it->lo - gap - 2;
    it->lo = it->hi - len;
    --it->left;
    *lo = it->lo; *hi = it->hi;
    return true;
}

proven_size_t proven_quic_ack_put(proven_quic_wr_t *w, const proven_u64 (*ranges)[2], proven_size_t count, proven_u64 ack_delay) {
    return proven_quic_ack_put_ecn(w, ranges, count, ack_delay, NULL);
}

proven_size_t proven_quic_ack_put_ecn(proven_quic_wr_t *w, const proven_u64 (*ranges)[2], proven_size_t count, proven_u64 ack_delay, const proven_u64 ecn[3]) {
    if (count == 0 || !w->ok) return 0;
    const proven_u64 largest = ranges[0][1], first = ranges[0][1] - ranges[0][0];
    const proven_size_t room = w->cap - w->n;
    proven_size_t need = 1 + proven_quic_varint_len(largest) + proven_quic_varint_len(ack_delay) + proven_quic_varint_len(first);
    if (ecn) for (int i = 0; i < 3; ++i) need += proven_quic_varint_len(ecn[i]);
    /* As many further ranges as fit, with the count's own length counted. */
    proven_size_t k = 0, pairs = 0;
    for (proven_size_t i = 1; i < count; ++i) {
        const proven_u64 gap = ranges[i - 1][0] - ranges[i][1] - 2, len = ranges[i][1] - ranges[i][0];
        const proven_size_t add = proven_quic_varint_len(gap) + proven_quic_varint_len(len);
        if (need + proven_quic_varint_len(k + 1) + pairs + add > room) break;
        pairs += add; ++k;
    }
    if (need + proven_quic_varint_len(k) + pairs > room) return 0;
    proven_quic_wr_varint(w, ecn ? PROVEN_QUIC_FT_ACK_ECN : PROVEN_QUIC_FT_ACK);
    proven_quic_wr_varint(w, largest);
    proven_quic_wr_varint(w, ack_delay);
    proven_quic_wr_varint(w, k);
    proven_quic_wr_varint(w, first);
    for (proven_size_t i = 1; i <= k; ++i) {
        proven_quic_wr_varint(w, ranges[i - 1][0] - ranges[i][1] - 2);
        proven_quic_wr_varint(w, ranges[i][1] - ranges[i][0]);
    }
    if (ecn) for (int i = 0; i < 3; ++i) proven_quic_wr_varint(w, ecn[i]);
    return w->ok ? k + 1 : 0;
}

#endif /* PROVEN_NO_HTTP3 */
