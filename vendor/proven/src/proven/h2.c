#include "proven/h2.h"

/*
 * HTTP/2 (RFC 9113): frames, and the rules for a header list. No state lives here.
 */

static proven_u32 h2_u32(const proven_byte_t *p) {
    return ((proven_u32)p[0] << 24) | ((proven_u32)p[1] << 16) | ((proven_u32)p[2] << 8) | (proven_u32)p[3];
}

static void h2_put_u32(proven_byte_t *p, proven_u32 v) {
    p[0] = (proven_byte_t)(v >> 24); p[1] = (proven_byte_t)(v >> 16); p[2] = (proven_byte_t)(v >> 8); p[3] = (proven_byte_t)v;
}

// -----------------------------------------------------------------------------
// Frames
// -----------------------------------------------------------------------------

proven_err_t proven_h2_frame_parse(proven_mem_view_t data, proven_h2_frame_t *out) {
    if (!out || (data.size > 0 && !data.ptr)) return PROVEN_ERR_INVALID_ARG;
    if (data.size < PROVEN_H2_FRAME_HEADER_SIZE) return PROVEN_ERR_NEED_MORE;
    const proven_byte_t *p = data.ptr;
    *out = (proven_h2_frame_t){
        .length = ((proven_u32)p[0] << 16) | ((proven_u32)p[1] << 8) | (proven_u32)p[2],
        .type = p[3],
        .flags = p[4],
        .stream_id = h2_u32(p + 5) & 0x7fffffffu,      /* the reserved bit is ignored on reading */
    };
    return PROVEN_OK;
}

proven_err_t proven_h2_frame_write(proven_mem_mut_t out, proven_size_t *len, const proven_h2_frame_t *frame) {
    if (!len || !frame || (out.size > 0 && !out.ptr) || *len > out.size) return PROVEN_ERR_INVALID_ARG;
    if (frame->length > PROVEN_H2_MAX_MAX_FRAME_SIZE || frame->stream_id > 0x7fffffffu) return PROVEN_ERR_INVALID_ARG;
    if (out.size - *len < PROVEN_H2_FRAME_HEADER_SIZE) return PROVEN_ERR_OUT_OF_BOUNDS;
    proven_byte_t *p = out.ptr + *len;
    p[0] = (proven_byte_t)(frame->length >> 16); p[1] = (proven_byte_t)(frame->length >> 8); p[2] = (proven_byte_t)frame->length;
    p[3] = frame->type;
    p[4] = frame->flags;
    h2_put_u32(p + 5, frame->stream_id);
    *len += PROVEN_H2_FRAME_HEADER_SIZE;
    return PROVEN_OK;
}

proven_err_t proven_h2_frame_check(const proven_h2_frame_t *frame, proven_u32 max_frame_size, proven_h2_error_t *code, bool *stream_error) {
    if (!frame || !code || !stream_error) return PROVEN_ERR_INVALID_ARG;
    *code = PROVEN_H2_NO_ERROR;
    *stream_error = false;
    proven_h2_error_t fault = PROVEN_H2_NO_ERROR;
    bool on_stream = false;
    const bool has_stream = frame->stream_id != 0;
    const proven_u32 n = frame->length;
    /* A frame that is too large is a connection error whatever it is: its payload may hold
     * state every later frame depends on, and cannot be skipped safely. */
    if (n > max_frame_size) fault = PROVEN_H2_FRAME_SIZE_ERROR;
    else switch (frame->type) {
        case PROVEN_H2_DATA:
        case PROVEN_H2_HEADERS:
        case PROVEN_H2_CONTINUATION:
        case PROVEN_H2_PUSH_PROMISE:
            if (!has_stream) fault = PROVEN_H2_PROTOCOL_ERROR;
            break;
        case PROVEN_H2_PRIORITY:
            if (!has_stream) fault = PROVEN_H2_PROTOCOL_ERROR;
            else if (n != 5) { fault = PROVEN_H2_FRAME_SIZE_ERROR; on_stream = true; }
            break;
        case PROVEN_H2_RST_STREAM:
            if (!has_stream) fault = PROVEN_H2_PROTOCOL_ERROR;
            else if (n != 4) fault = PROVEN_H2_FRAME_SIZE_ERROR;
            break;
        case PROVEN_H2_SETTINGS:
            if (has_stream) fault = PROVEN_H2_PROTOCOL_ERROR;
            else if ((frame->flags & PROVEN_H2_FLAG_ACK) != 0 ? n != 0 : n % 6 != 0) fault = PROVEN_H2_FRAME_SIZE_ERROR;
            break;
        case PROVEN_H2_PING:
            if (has_stream) fault = PROVEN_H2_PROTOCOL_ERROR;
            else if (n != 8) fault = PROVEN_H2_FRAME_SIZE_ERROR;
            break;
        case PROVEN_H2_GOAWAY:
            if (has_stream) fault = PROVEN_H2_PROTOCOL_ERROR;
            else if (n < 8) fault = PROVEN_H2_FRAME_SIZE_ERROR;
            break;
        case PROVEN_H2_WINDOW_UPDATE:
            if (n != 4) fault = PROVEN_H2_FRAME_SIZE_ERROR;
            break;
        default:
            break;                     /* unknown types are ignored, by the protocol's own rule */
    }
    if (fault == PROVEN_H2_NO_ERROR) return PROVEN_OK;
    *code = fault;
    *stream_error = on_stream;
    return PROVEN_ERR_INVALID_FORMAT;
}

proven_err_t proven_h2_frame_content(const proven_h2_frame_t *frame, proven_mem_view_t payload, proven_mem_view_t *content) {
    if (!frame || !content || (payload.size > 0 && !payload.ptr) || payload.size != frame->length) return PROVEN_ERR_INVALID_ARG;
    if (frame->type != PROVEN_H2_DATA && frame->type != PROVEN_H2_HEADERS) return PROVEN_ERR_INVALID_ARG;
    proven_size_t at = 0, end = payload.size;
    if ((frame->flags & PROVEN_H2_FLAG_PADDED) != 0) {
        if (end < 1) return PROVEN_ERR_INVALID_FORMAT;
        proven_size_t pad = payload.ptr[0];
        at = 1;
        /* "Padding that exceeds the size remaining for the content" - the pad byte itself is
         * not content. */
        if (pad > end - at) return PROVEN_ERR_INVALID_FORMAT;
        end -= pad;
    }
    if (frame->type == PROVEN_H2_HEADERS && (frame->flags & PROVEN_H2_FLAG_PRIORITY) != 0) {
        if (end - at < 5) return PROVEN_ERR_INVALID_FORMAT;
        at += 5;
    }
    *content = (proven_mem_view_t){ .ptr = payload.ptr + at, .size = end - at };
    return PROVEN_OK;
}

proven_err_t proven_h2_settings_next(proven_mem_view_t payload, proven_size_t *pos, proven_h2_setting_t *out, proven_h2_error_t *code) {
    if (!pos || !out || !code || (payload.size > 0 && !payload.ptr) || *pos > payload.size) return PROVEN_ERR_INVALID_ARG;
    *code = PROVEN_H2_NO_ERROR;
    if (*pos == payload.size) return PROVEN_ERR_EOF;
    if (payload.size - *pos < 6) { *code = PROVEN_H2_FRAME_SIZE_ERROR; return PROVEN_ERR_INVALID_FORMAT; }
    const proven_byte_t *p = payload.ptr + *pos;
    proven_h2_setting_t s = { .id = (proven_u16)(((proven_u16)p[0] << 8) | p[1]), .value = h2_u32(p + 2) };
    if ((s.id == PROVEN_H2_SETTINGS_ENABLE_PUSH || s.id == PROVEN_H2_SETTINGS_ENABLE_CONNECT_PROTOCOL) && s.value > 1) *code = PROVEN_H2_PROTOCOL_ERROR;
    else if (s.id == PROVEN_H2_SETTINGS_INITIAL_WINDOW_SIZE && s.value > PROVEN_H2_MAX_WINDOW) *code = PROVEN_H2_FLOW_CONTROL_ERROR;
    else if (s.id == PROVEN_H2_SETTINGS_MAX_FRAME_SIZE && (s.value < PROVEN_H2_MIN_MAX_FRAME_SIZE || s.value > PROVEN_H2_MAX_MAX_FRAME_SIZE)) *code = PROVEN_H2_PROTOCOL_ERROR;
    if (*code != PROVEN_H2_NO_ERROR) return PROVEN_ERR_INVALID_FORMAT;
    *out = s;
    *pos += 6;
    return PROVEN_OK;
}

/* A whole small frame: header, then `n` payload bytes from `payload`. */
static proven_err_t h2_write(proven_mem_mut_t out, proven_size_t *len, proven_u8 type, proven_u8 flags, proven_u32 stream_id, const proven_byte_t *payload, proven_size_t n) {
    if (!len || (out.size > 0 && !out.ptr) || *len > out.size) return PROVEN_ERR_INVALID_ARG;
    if (out.size - *len < PROVEN_H2_FRAME_HEADER_SIZE + n) return PROVEN_ERR_OUT_OF_BOUNDS;
    proven_h2_frame_t f = { .length = (proven_u32)n, .type = type, .flags = flags, .stream_id = stream_id };
    proven_err_t e = proven_h2_frame_write(out, len, &f);
    if (e != PROVEN_OK) return e;
    for (proven_size_t i = 0; i < n; ++i) out.ptr[*len + i] = payload[i];
    *len += n;
    return PROVEN_OK;
}

proven_err_t proven_h2_write_settings(proven_mem_mut_t out, proven_size_t *len, const proven_h2_setting_t *settings, proven_size_t count) {
    if (!len || (count > 0 && !settings) || (out.size > 0 && !out.ptr) || *len > out.size || count > 1024) return PROVEN_ERR_INVALID_ARG;
    if (out.size - *len < PROVEN_H2_FRAME_HEADER_SIZE + count * 6) return PROVEN_ERR_OUT_OF_BOUNDS;
    proven_h2_frame_t f = { .length = (proven_u32)(count * 6), .type = PROVEN_H2_SETTINGS };
    proven_err_t e = proven_h2_frame_write(out, len, &f);
    if (e != PROVEN_OK) return e;
    for (proven_size_t i = 0; i < count; ++i) {
        proven_byte_t *p = out.ptr + *len;
        p[0] = (proven_byte_t)(settings[i].id >> 8); p[1] = (proven_byte_t)settings[i].id;
        h2_put_u32(p + 2, settings[i].value);
        *len += 6;
    }
    return PROVEN_OK;
}

proven_err_t proven_h2_write_settings_ack(proven_mem_mut_t out, proven_size_t *len) {
    return h2_write(out, len, PROVEN_H2_SETTINGS, PROVEN_H2_FLAG_ACK, 0, (void *)0, 0);
}

proven_err_t proven_h2_write_ping(proven_mem_mut_t out, proven_size_t *len, const proven_byte_t opaque[8], bool ack) {
    if (!opaque) return PROVEN_ERR_INVALID_ARG;
    return h2_write(out, len, PROVEN_H2_PING, ack ? PROVEN_H2_FLAG_ACK : 0, 0, opaque, 8);
}

proven_err_t proven_h2_write_window_update(proven_mem_mut_t out, proven_size_t *len, proven_u32 stream_id, proven_u32 increment) {
    if (increment == 0 || increment > PROVEN_H2_MAX_WINDOW || stream_id > 0x7fffffffu) return PROVEN_ERR_INVALID_ARG;
    proven_byte_t p[4];
    h2_put_u32(p, increment);
    return h2_write(out, len, PROVEN_H2_WINDOW_UPDATE, 0, stream_id, p, 4);
}

proven_err_t proven_h2_write_rst_stream(proven_mem_mut_t out, proven_size_t *len, proven_u32 stream_id, proven_h2_error_t code) {
    if (stream_id == 0 || stream_id > 0x7fffffffu) return PROVEN_ERR_INVALID_ARG;
    proven_byte_t p[4];
    h2_put_u32(p, (proven_u32)code);
    return h2_write(out, len, PROVEN_H2_RST_STREAM, 0, stream_id, p, 4);
}

proven_err_t proven_h2_write_goaway(proven_mem_mut_t out, proven_size_t *len, proven_u32 last_stream_id, proven_h2_error_t code, proven_mem_view_t debug) {
    if (!len || (out.size > 0 && !out.ptr) || *len > out.size || (debug.size > 0 && !debug.ptr) || last_stream_id > 0x7fffffffu || debug.size > 16384 - 8) return PROVEN_ERR_INVALID_ARG;
    if (out.size - *len < PROVEN_H2_FRAME_HEADER_SIZE + 8 + debug.size) return PROVEN_ERR_OUT_OF_BOUNDS;
    proven_byte_t p[8];
    h2_put_u32(p, last_stream_id);
    h2_put_u32(p + 4, (proven_u32)code);
    proven_h2_frame_t f = { .length = (proven_u32)(8 + debug.size), .type = PROVEN_H2_GOAWAY };
    proven_err_t e = proven_h2_frame_write(out, len, &f);
    if (e != PROVEN_OK) return e;
    for (proven_size_t i = 0; i < 8; ++i) out.ptr[*len + i] = p[i];
    for (proven_size_t i = 0; i < debug.size; ++i) out.ptr[*len + 8 + i] = debug.ptr[i];
    *len += 8 + debug.size;
    return PROVEN_OK;
}

proven_err_t proven_h2_read_window_update(proven_mem_view_t payload, proven_u32 *increment) {
    if (!increment || payload.size != 4 || !payload.ptr) return PROVEN_ERR_INVALID_ARG;
    *increment = h2_u32(payload.ptr) & 0x7fffffffu;
    return *increment == 0 ? PROVEN_ERR_INVALID_FORMAT : PROVEN_OK;
}

proven_err_t proven_h2_read_rst_stream(proven_mem_view_t payload, proven_u32 *code) {
    if (!code || payload.size != 4 || !payload.ptr) return PROVEN_ERR_INVALID_ARG;
    *code = h2_u32(payload.ptr);
    return PROVEN_OK;
}

proven_err_t proven_h2_read_goaway(proven_mem_view_t payload, proven_u32 *last_stream_id, proven_u32 *code, proven_mem_view_t *debug) {
    if (!last_stream_id || !code || !debug || payload.size < 8 || !payload.ptr) return PROVEN_ERR_INVALID_ARG;
    *last_stream_id = h2_u32(payload.ptr) & 0x7fffffffu;
    *code = h2_u32(payload.ptr + 4);
    *debug = (proven_mem_view_t){ .ptr = payload.ptr + 8, .size = payload.size - 8 };
    return PROVEN_OK;
}

// -----------------------------------------------------------------------------
// A header list as a message
// -----------------------------------------------------------------------------

static bool h2_same(const proven_byte_t *p, const char *lit, proven_size_t n) {
    for (proven_size_t i = 0; i < n; ++i) if (p[i] != (proven_byte_t)lit[i]) return false;
    return true;
}
/* `lit` is a string literal: its length is known where this is written, and is compared first. */
#define h2_is(s, lit) ((s).size == sizeof(lit) - 1 && h2_same((s).ptr, (lit), sizeof(lit) - 1))

/* What a byte may not be in (RFC 9113, 8.2.1): 1 - a field name (a control, a space, an upper-case
 * letter, DEL or above); 2 - a field value (NUL, LF, CR); 4 - it is a colon. */
enum { H2_BAD_IN_NAME = 1, H2_BAD_IN_VALUE = 2, H2_COLON = 4 };
static const proven_u8 h2_class[256] = {
    3,1,1,1,1,1,1,1,1,1,3,1,1,3,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
    1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,4,0,0,0,0,0,
    0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
};

/* RFC 9113, 8.2.1: the minimal validation every endpoint must do. `pseudo`: the name may
 * begin with a colon (and only begin with one). */
static bool h2_field_ok(const proven_http_header_t *h, bool pseudo) {
    if (h->name.size == 0 || (h->name.size > 0 && !h->name.ptr) || (h->value.size > 0 && !h->value.ptr)) return false;
    /* One table look-up a byte, and the verdict taken at the end of each string. */
    unsigned seen = 0;
    for (proven_size_t i = 0; i < h->name.size; ++i) seen |= h2_class[h->name.ptr[i]];
    if ((seen & H2_BAD_IN_NAME) != 0) return false;
    if ((seen & H2_COLON) != 0) {
        /* A colon only begins a name, and only a pseudo-header field's. */
        if (!pseudo || h->name.ptr[0] != ':') return false;
        for (proven_size_t i = 1; i < h->name.size; ++i) if (h->name.ptr[i] == ':') return false;
    }
    seen = 0;
    for (proven_size_t i = 0; i < h->value.size; ++i) seen |= h2_class[h->value.ptr[i]];
    if ((seen & H2_BAD_IN_VALUE) != 0) return false;
    if (h->value.size > 0) {
        proven_byte_t a = h->value.ptr[0], z = h->value.ptr[h->value.size - 1];
        if (a == ' ' || a == '\t' || z == ' ' || z == '\t') return false;
    }
    return true;
}

/* RFC 9113, 8.2.2: fields that describe a connection have no place here. */
static bool h2_ordinary_ok(const proven_http_header_t *h, bool request) {
    if (!h2_field_ok(h, false)) return false;
    switch (h->name.size) {                       /* by length first: most names are none of these */
        case 2: return !h2_is(h->name, "te") || (request && h2_is(h->value, "trailers"));
        case 7: return !h2_is(h->name, "upgrade");
        case 10: return !h2_is(h->name, "connection") && !h2_is(h->name, "keep-alive");
        case 16: return !h2_is(h->name, "proxy-connection");
        case 17: return !h2_is(h->name, "transfer-encoding");
        default: return true;
    }
}

proven_err_t proven_h2_request_head(const proven_http_header_t *headers, proven_size_t count, proven_h2_request_head_t *out) {
    if (!out || (count > 0 && !headers)) return PROVEN_ERR_INVALID_ARG;
    *out = (proven_h2_request_head_t){ 0 };
    bool have_method = false, have_scheme = false, have_authority = false, have_path = false, have_protocol = false;
    proven_size_t i = 0;
    for (; i < count && headers[i].name.size > 0 && headers[i].name.ptr && headers[i].name.ptr[0] == ':'; ++i) {
        const proven_http_header_t *h = &headers[i];
        if (!h2_field_ok(h, true)) return PROVEN_ERR_INVALID_FORMAT;
        if (h2_is(h->name, ":method")) { if (have_method) return PROVEN_ERR_INVALID_FORMAT; have_method = true; out->method = h->value; }
        else if (h2_is(h->name, ":scheme")) { if (have_scheme) return PROVEN_ERR_INVALID_FORMAT; have_scheme = true; out->scheme = h->value; }
        else if (h2_is(h->name, ":authority")) { if (have_authority) return PROVEN_ERR_INVALID_FORMAT; have_authority = true; out->authority = h->value; }
        else if (h2_is(h->name, ":path")) { if (have_path) return PROVEN_ERR_INVALID_FORMAT; have_path = true; out->path = h->value; }
        else if (h2_is(h->name, ":protocol")) { if (have_protocol) return PROVEN_ERR_INVALID_FORMAT; have_protocol = true; out->protocol = h->value; }
        else return PROVEN_ERR_INVALID_FORMAT;         /* :status, or one nobody defined */
    }
    out->fields = headers + i;
    out->field_count = count - i;
    for (; i < count; ++i) {
        /* An ordinary field - and so no pseudo-header field may follow. */
        if (!h2_ordinary_ok(&headers[i], true)) return PROVEN_ERR_INVALID_FORMAT;
    }
    if (!have_method || out->method.size == 0) return PROVEN_ERR_INVALID_FORMAT;
    if (have_protocol) {
        /* RFC 8441, section 4: an extended CONNECT names a protocol, and its target is a URI -
         * scheme, authority and path all there. Only CONNECT takes it. */
        if (!h2_is(out->method, "CONNECT") || out->protocol.size == 0) return PROVEN_ERR_INVALID_FORMAT;
        if (!have_scheme || out->scheme.size == 0 || !have_path || out->path.size == 0 || !have_authority || out->authority.size == 0) return PROVEN_ERR_INVALID_FORMAT;
        return PROVEN_OK;
    }
    if (h2_is(out->method, "CONNECT")) {
        /* RFC 9113, 8.5: the authority is the target, and there is no scheme or path. */
        if (!have_authority || out->authority.size == 0 || have_scheme || have_path) return PROVEN_ERR_INVALID_FORMAT;
        return PROVEN_OK;
    }
    if (!have_scheme || out->scheme.size == 0 || !have_path) return PROVEN_ERR_INVALID_FORMAT;
    if (out->path.size == 0 && (h2_is(out->scheme, "http") || h2_is(out->scheme, "https"))) return PROVEN_ERR_INVALID_FORMAT;
    return PROVEN_OK;
}

proven_err_t proven_h2_response_head(const proven_http_header_t *headers, proven_size_t count, proven_u16 *status,
                                     const proven_http_header_t **fields, proven_size_t *field_count) {
    if (!status || !fields || !field_count || (count > 0 && !headers)) return PROVEN_ERR_INVALID_ARG;
    *status = 0;
    *fields = (void *)0;
    *field_count = 0;
    if (count == 0 || !h2_is(headers[0].name, ":status") || !h2_field_ok(&headers[0], true)) return PROVEN_ERR_INVALID_FORMAT;
    proven_u8str_view_t v = headers[0].value;
    if (v.size != 3) return PROVEN_ERR_INVALID_FORMAT;
    proven_u16 code = 0;
    for (proven_size_t i = 0; i < 3; ++i) {
        if (v.ptr[i] < '0' || v.ptr[i] > '9') return PROVEN_ERR_INVALID_FORMAT;
        code = (proven_u16)(code * 10 + (v.ptr[i] - '0'));
    }
    if (code < 100) return PROVEN_ERR_INVALID_FORMAT;
    for (proven_size_t i = 1; i < count; ++i) {
        if (!h2_ordinary_ok(&headers[i], false)) return PROVEN_ERR_INVALID_FORMAT;    /* a second pseudo-header field is refused as a name with a colon */
    }
    *status = code;
    *fields = headers + 1;
    *field_count = count - 1;
    return PROVEN_OK;
}

proven_err_t proven_h2_trailers_check(const proven_http_header_t *headers, proven_size_t count) {
    if (count > 0 && !headers) return PROVEN_ERR_INVALID_ARG;
    for (proven_size_t i = 0; i < count; ++i) {
        if (!h2_ordinary_ok(&headers[i], false)) return PROVEN_ERR_INVALID_FORMAT;
    }
    return PROVEN_OK;
}
