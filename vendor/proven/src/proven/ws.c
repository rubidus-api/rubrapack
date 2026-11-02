#include "proven/ws.h"
#include "proven_internal_ws.h"
#include "proven/hash_legacy.h"
#include "proven/encode.h"
#include "proven/utf.h"

/*
 * RFC 6455. Pure: the handshake arithmetic, the frame header, and the decoder that applies the
 * receiver's rules. Nothing here reads, writes, allocates or draws a random number.
 */

static proven_u8str_view_t ws_lit(const char *s) {
    proven_size_t n = 0;
    while (s[n] != '\0') n++;
    return (proven_u8str_view_t){ .ptr = (const proven_byte_t *)s, .size = n };
}

static bool ws_eq(proven_u8str_view_t a, proven_u8str_view_t b) {
    if (a.size != b.size) return false;
    for (proven_size_t i = 0; i < a.size; ++i) if (a.ptr[i] != b.ptr[i]) return false;
    return true;
}

static proven_u8str_view_t ws_trim(proven_u8str_view_t s) {
    while (s.size > 0 && (s.ptr[0] == ' ' || s.ptr[0] == '\t')) { s.ptr++; s.size--; }
    while (s.size > 0 && (s.ptr[s.size - 1] == ' ' || s.ptr[s.size - 1] == '\t')) s.size--;
    return s;
}

/* Whether the comma-separated `list` holds `name`, compared exactly. */
static bool ws_list_has(proven_u8str_view_t list, proven_u8str_view_t name) {
    proven_size_t start = 0;
    for (proven_size_t i = 0; i <= list.size; ++i) {
        if (i < list.size && list.ptr[i] != ',') continue;
        if (ws_eq(ws_trim((proven_u8str_view_t){ .ptr = list.ptr + start, .size = i - start }), name)) return true;
        start = i + 1;
    }
    return false;
}

/* Whole-text UTF-8 check. */
static bool ws_utf8_ok(proven_u8str_view_t s) {
    proven_size_t pos = 0;
    while (pos < s.size) {
        proven_utf8_char_t c = proven_utf8_decode_next(s, pos);
        if (c.err != PROVEN_OK) return false;
        pos += c.len;
    }
    return true;
}

// -----------------------------------------------------------------------------
// The opening handshake
// -----------------------------------------------------------------------------

void proven_ws_make_key(const proven_byte_t random[16], proven_byte_t out[PROVEN_WS_KEY_SIZE]) {
    proven_size_t n = 0;
    /* 16 bytes are always 24 characters, so this cannot fail. */
    (void)proven_base64_encode((proven_mem_view_t){ .ptr = random, .size = 16 }, out, PROVEN_WS_KEY_SIZE, &n);
}

proven_err_t proven_ws_accept_key(proven_u8str_view_t key, proven_byte_t out[PROVEN_WS_ACCEPT_SIZE]) {
    static const char guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    if (!out || key.size != PROVEN_WS_KEY_SIZE || !key.ptr) return PROVEN_ERR_INVALID_FORMAT;
    proven_byte_t raw[18];
    proven_size_t n = 0;
    if (proven_base64_decode((proven_mem_view_t){ .ptr = key.ptr, .size = key.size }, raw, sizeof raw, &n) != PROVEN_OK || n != 16) {
        return PROVEN_ERR_INVALID_FORMAT;
    }
    proven_byte_t joined[PROVEN_WS_KEY_SIZE + 36];
    for (proven_size_t i = 0; i < PROVEN_WS_KEY_SIZE; ++i) joined[i] = key.ptr[i];
    for (proven_size_t i = 0; i < 36; ++i) joined[PROVEN_WS_KEY_SIZE + i] = (proven_byte_t)guid[i];
    proven_byte_t digest[PROVEN_SHA1_SIZE];
    proven_sha1((proven_mem_view_t){ .ptr = joined, .size = sizeof joined }, digest);
    return proven_base64_encode((proven_mem_view_t){ .ptr = digest, .size = sizeof digest }, out, PROVEN_WS_ACCEPT_SIZE, &n);
}


proven_err_t proven_ws_check_request(const proven_http_request_t *request, proven_u8str_view_t *key) {
    if (!request || !key) return PROVEN_ERR_INVALID_ARG;
    const proven_http_header_t *h = request->headers;
    proven_size_t n = request->header_count;
    /* HTTP/2 (RFC 8441): an extended CONNECT, which the event-driven server presents with a
     * `:protocol` field first. It has no key: the stream itself is the proof. */
    if (request->method == PROVEN_HTTP_CONNECT && n > 0 && proven_u8str_view_eq(h[0].name, ws_lit(":protocol"))) {
        if (!proven_u8str_view_eq_ascii_ci(h[0].value, ws_lit("websocket"))) return PROVEN_ERR_NOT_FOUND;
        proven_u8str_view_t version;
        if (proven_http_header_count(h, n, ws_lit("Sec-WebSocket-Version")) != 1 ||
            !proven_http_header_find(h, n, ws_lit("Sec-WebSocket-Version"), &version)) return PROVEN_ERR_INVALID_FORMAT;
        if (!ws_eq(ws_trim(version), ws_lit("13"))) return PROVEN_ERR_UNSUPPORTED;
        *key = (proven_u8str_view_t){ .ptr = (const proven_byte_t *)"", .size = 0 };
        return PROVEN_OK;
    }
    if (!proven_http_header_has_token(h, n, ws_lit("Upgrade"), ws_lit("websocket"))) return PROVEN_ERR_NOT_FOUND;
    if (request->method != PROVEN_HTTP_GET || request->version_minor < 1) return PROVEN_ERR_INVALID_FORMAT;
    if (!proven_http_header_has_token(h, n, ws_lit("Connection"), ws_lit("Upgrade"))) return PROVEN_ERR_INVALID_FORMAT;
    proven_u8str_view_t version;
    if (proven_http_header_count(h, n, ws_lit("Sec-WebSocket-Version")) != 1 ||
        !proven_http_header_find(h, n, ws_lit("Sec-WebSocket-Version"), &version)) return PROVEN_ERR_INVALID_FORMAT;
    if (!ws_eq(ws_trim(version), ws_lit("13"))) return PROVEN_ERR_UNSUPPORTED;
    proven_u8str_view_t k;
    if (proven_http_header_count(h, n, ws_lit("Sec-WebSocket-Key")) != 1 ||
        !proven_http_header_find(h, n, ws_lit("Sec-WebSocket-Key"), &k)) return PROVEN_ERR_INVALID_FORMAT;
    k = ws_trim(k);
    proven_byte_t probe[PROVEN_WS_ACCEPT_SIZE];
    if (proven_ws_accept_key(k, probe) != PROVEN_OK) return PROVEN_ERR_INVALID_FORMAT;
    *key = k;
    return PROVEN_OK;
}


proven_err_t proven_ws_check_origin(const proven_http_request_t *request, const proven_u8str_view_t *allowed, proven_size_t allowed_count) {
    if (!request || (!allowed && allowed_count > 0)) return PROVEN_ERR_INVALID_ARG;
    static const proven_u8str_view_t field = { .ptr = (const proven_byte_t *)"origin", .size = 6 };
    const proven_u8str_view_t *origin = (void *)0;
    for (proven_size_t i = 0; i < request->header_count; ++i) {
        if (!proven_u8str_view_eq_ascii_ci(request->headers[i].name, field)) continue;
        if (origin) return PROVEN_ERR_PERMISSION;          /* two of them: which one is true? */
        origin = &request->headers[i].value;
    }
    if (!origin) return PROVEN_ERR_NOT_FOUND;
    for (proven_size_t i = 0; i < allowed_count; ++i) {
        if (allowed[i].size > 0 && proven_u8str_view_eq_ascii_ci(*origin, allowed[i])) return PROVEN_OK;
    }
    return PROVEN_ERR_PERMISSION;
}

bool proven_ws_request_offers(const proven_http_request_t *request, proven_u8str_view_t name) {
    if (!request || name.size == 0) return false;
    for (proven_size_t i = 0; i < request->header_count; ++i) {
        proven_u8str_view_t hn = request->headers[i].name;
        static const char want[] = "sec-websocket-protocol";
        if (hn.size != sizeof want - 1) continue;
        bool same = true;
        for (proven_size_t k = 0; k < hn.size && same; ++k) {
            proven_byte_t c = hn.ptr[k];
            if (c >= 'A' && c <= 'Z') c = (proven_byte_t)(c + 32);
            same = c == (proven_byte_t)want[k];
        }
        if (same && ws_list_has(request->headers[i].value, name)) return true;
    }
    return false;
}

/* One `permessage-deflate` element of Sec-WebSocket-Extensions, taken apart. */
typedef struct {
    bool server_no_takeover, client_no_takeover, server_bits_seen, client_bits_seen;
    int server_bits, client_bits;      /* 15 when not given (or, for the client's in an offer, given bare) */
    bool whole;                        /* the element was the only thing in the value */
} proven_ws_deflate_params_t;


static bool ws_eq_nocase(proven_u8str_view_t a, const char *b) { return proven_u8str_view_eq_ascii_ci(a, proven_u8str_view_from_cstr(b)); }

/* A window size as a parameter value: 8 to 15, bare or in quotes. 0 when it is not one. */
static int ws_window_value(proven_u8str_view_t v) {
    if (v.size >= 2 && v.ptr[0] == '"' && v.ptr[v.size - 1] == '"') { v.ptr++; v.size -= 2; }
    if (v.size == 1 && (v.ptr[0] == '8' || v.ptr[0] == '9')) return v.ptr[0] - '0';
    if (v.size == 2 && v.ptr[0] == '1' && v.ptr[1] >= '0' && v.ptr[1] <= '5') return 10 + (v.ptr[1] - '0');
    return 0;
}

/* Parse the first element of `list` (up to a comma) as a permessage-deflate element. False
 * when it is another extension or not well formed: a parameter that is unknown or given
 * twice, a value where none belongs or none where one does (`client_max_window_bits` may go
 * without one in an offer only). `*rest`, when given, receives what follows the comma. */
static bool ws_deflate_element(proven_u8str_view_t list, bool offer, proven_ws_deflate_params_t *out, proven_u8str_view_t *rest) {
    proven_size_t end = 0;
    while (end < list.size && list.ptr[end] != ',') end++;
    if (rest) *rest = (proven_u8str_view_t){ .ptr = list.ptr + (end < list.size ? end + 1 : end), .size = list.size - (end < list.size ? end + 1 : end) };
    proven_u8str_view_t element = { .ptr = list.ptr, .size = end };
    *out = (proven_ws_deflate_params_t){ .server_bits = 15, .client_bits = 15 };
    bool named = false;
    proven_size_t start = 0;
    for (proven_size_t i = 0; i <= element.size; ++i) {
        if (i < element.size && element.ptr[i] != ';') continue;
        proven_u8str_view_t part = ws_trim((proven_u8str_view_t){ .ptr = element.ptr + start, .size = i - start });
        start = i + 1;
        if (!named) {
            if (!ws_eq_nocase(part, "permessage-deflate")) return false;
            named = true;
            continue;
        }
        proven_size_t eq = 0;
        while (eq < part.size && part.ptr[eq] != '=') eq++;
        proven_u8str_view_t name = ws_trim((proven_u8str_view_t){ .ptr = part.ptr, .size = eq });
        bool has_value = eq < part.size;
        proven_u8str_view_t value = has_value ? ws_trim((proven_u8str_view_t){ .ptr = part.ptr + eq + 1, .size = part.size - eq - 1 }) : (proven_u8str_view_t){ 0 };
        if (ws_eq_nocase(name, "server_no_context_takeover")) {
            if (has_value || out->server_no_takeover) return false;
            out->server_no_takeover = true;
        } else if (ws_eq_nocase(name, "client_no_context_takeover")) {
            if (has_value || out->client_no_takeover) return false;
            out->client_no_takeover = true;
        } else if (ws_eq_nocase(name, "server_max_window_bits")) {
            int bits = has_value ? ws_window_value(value) : 0;
            if (bits == 0 || out->server_bits_seen) return false;
            out->server_bits_seen = true;
            out->server_bits = bits;
        } else if (ws_eq_nocase(name, "client_max_window_bits")) {
            int bits = has_value ? ws_window_value(value) : 0;
            if (out->client_bits_seen || (has_value ? bits == 0 : !offer)) return false;
            out->client_bits_seen = true;
            if (bits != 0) out->client_bits = bits;
        } else {
            return false;
        }
    }
    return named;
}

/* A whole header value that must be exactly one permessage-deflate element (a server's answer). */
static bool ws_deflate_parse(proven_u8str_view_t value, bool offer, proven_ws_deflate_params_t *out) {
    proven_u8str_view_t rest;
    if (!ws_deflate_element(value, offer, out, &rest)) return false;
    out->whole = ws_trim(rest).size == 0 && (value.size == 0 || value.ptr[value.size - 1] != ',');
    return true;
}

/* The three servers' choice. WS_ANSWER_PLAIN forbids takeover both ways, as it always has;
 * WS_ANSWER_TAKEOVER only where the offer asked; WS_ANSWER_PEER_NONE the client's always and
 * the server's where the offer asked. */
enum { WS_ANSWER_PLAIN, WS_ANSWER_TAKEOVER, WS_ANSWER_PEER_NONE };
static proven_err_t ws_deflate_answer(const proven_http_request_t *request, int mode, proven_mem_mut_t out, proven_size_t *len,
                                      proven_ws_deflate_terms_t *terms) {
    for (proven_size_t i = 0; i < request->header_count; ++i) {
        if (!ws_eq_nocase(request->headers[i].name, "sec-websocket-extensions")) continue;
        proven_u8str_view_t rest = request->headers[i].value;
        while (ws_trim(rest).size > 0) {
            proven_ws_deflate_params_t p;
            proven_u8str_view_t next;
            bool ok = ws_deflate_element(rest, true, &p, &next);
            rest = next;
            /* This compressor's smallest window is 512 bytes; an offer that insists on 256
             * is one for the next offer, or for no compression. */
            if (!ok || p.server_bits < 9) continue;
            static const char name[] = "permessage-deflate";
            static const char server_no[] = "; server_no_context_takeover";
            static const char client_no[] = "; client_no_context_takeover";
            static const char bits[] = "; server_max_window_bits=";
            bool say_server = mode == WS_ANSWER_PLAIN || p.server_no_takeover, say_client = mode != WS_ANSWER_TAKEOVER || p.client_no_takeover;
            proven_size_t need = sizeof name - 1 + (say_server ? sizeof server_no - 1 : 0) + (say_client ? sizeof client_no - 1 : 0) +
                                 (p.server_bits_seen ? sizeof bits - 1 + 2 : 0);
            if (out.size - *len < need) return PROVEN_ERR_OUT_OF_BOUNDS;
            proven_size_t at = *len;
            for (proven_size_t k = 0; k < sizeof name - 1; ++k) out.ptr[at++] = (proven_byte_t)name[k];
            for (proven_size_t k = 0; say_server && k < sizeof server_no - 1; ++k) out.ptr[at++] = (proven_byte_t)server_no[k];
            for (proven_size_t k = 0; say_client && k < sizeof client_no - 1; ++k) out.ptr[at++] = (proven_byte_t)client_no[k];
            if (p.server_bits_seen) {
                for (proven_size_t k = 0; k < sizeof bits - 1; ++k) out.ptr[at++] = (proven_byte_t)bits[k];
                if (p.server_bits >= 10) out.ptr[at++] = '1';
                out.ptr[at++] = (proven_byte_t)('0' + p.server_bits % 10);
            }
            *len = at;
            *terms = (proven_ws_deflate_terms_t){ .server_takeover = !say_server, .client_takeover = !say_client,
                                                  .server_window_bits = p.server_bits, .client_window_bits = 15 };
            return PROVEN_OK;
        }
    }
    return PROVEN_ERR_NOT_FOUND;
}

proven_err_t proven_ws_deflate_accept(const proven_http_request_t *request, proven_mem_mut_t out, proven_size_t *len, int *window_bits) {
    if (!request || !len || !window_bits || (out.size > 0 && !out.ptr) || *len > out.size) return PROVEN_ERR_INVALID_ARG;
    proven_ws_deflate_terms_t terms;
    proven_err_t e = ws_deflate_answer(request, WS_ANSWER_PLAIN, out, len, &terms);
    if (e == PROVEN_OK) *window_bits = terms.server_window_bits;
    return e;
}

proven_err_t proven_ws_deflate_accept_takeover(const proven_http_request_t *request, proven_mem_mut_t out, proven_size_t *len,
                                               proven_ws_deflate_terms_t *terms) {
    if (!request || !len || !terms || (out.size > 0 && !out.ptr) || *len > out.size) return PROVEN_ERR_INVALID_ARG;
    return ws_deflate_answer(request, WS_ANSWER_TAKEOVER, out, len, terms);
}

proven_err_t proven_ws_deflate_accept_peer_no_takeover(const proven_http_request_t *request, proven_mem_mut_t out, proven_size_t *len,
                                                       proven_ws_deflate_terms_t *terms) {
    if (!request || !len || !terms || (out.size > 0 && !out.ptr) || *len > out.size) return PROVEN_ERR_INVALID_ARG;
    return ws_deflate_answer(request, WS_ANSWER_PEER_NONE, out, len, terms);
}

/* `deflate` null: nothing was offered. `terms` null: PROVEN_WS_DEFLATE_OFFER was; otherwise
 * PROVEN_WS_DEFLATE_OFFER_TAKEOVER, or with `server_none` PROVEN_WS_DEFLATE_OFFER_PEER_NO_TAKEOVER,
 * whose `server_no_context_takeover` an answer must keep (RFC 7692 section 7.1.1.1). */
static proven_err_t ws_check_response(const proven_http_response_t *response, proven_u8str_view_t key,
                                     proven_u8str_view_t offered, proven_u8str_view_t *protocol, bool *deflate,
                                     proven_ws_deflate_terms_t *terms, bool server_none) {
    if (!response || !protocol) return PROVEN_ERR_INVALID_ARG;
    *protocol = (proven_u8str_view_t){0};
    const proven_http_header_t *h = response->headers;
    proven_size_t n = response->header_count;
    if (response->status != 101) return PROVEN_ERR_INVALID_FORMAT;
    if (!proven_http_header_has_token(h, n, ws_lit("Upgrade"), ws_lit("websocket")) ||
        !proven_http_header_has_token(h, n, ws_lit("Connection"), ws_lit("Upgrade"))) return PROVEN_ERR_INVALID_FORMAT;
    proven_byte_t want[PROVEN_WS_ACCEPT_SIZE];
    proven_u8str_view_t got;
    if (proven_ws_accept_key(key, want) != PROVEN_OK) return PROVEN_ERR_INVALID_ARG;
    if (proven_http_header_count(h, n, ws_lit("Sec-WebSocket-Accept")) != 1 ||
        !proven_http_header_find(h, n, ws_lit("Sec-WebSocket-Accept"), &got) ||
        !ws_eq(ws_trim(got), (proven_u8str_view_t){ .ptr = want, .size = sizeof want })) return PROVEN_ERR_INVALID_FORMAT;
    proven_size_t extensions = proven_http_header_count(h, n, ws_lit("Sec-WebSocket-Extensions"));
    if (extensions != 0) {
        /* Only a client that offered permessage-deflate may be answered with it, once, in a
         * form that was offered: without takeover, the server must keep nothing between
         * messages, and the client's window was not put up for negotiation. */
        proven_u8str_view_t value;
        if (!deflate || extensions != 1 || !proven_http_header_find(h, n, ws_lit("Sec-WebSocket-Extensions"), &value)) return PROVEN_ERR_INVALID_FORMAT;
        proven_ws_deflate_params_t params;
        if (!ws_deflate_parse(value, false, &params) || !params.whole) return PROVEN_ERR_INVALID_FORMAT;
        if ((!terms || server_none) && !params.server_no_takeover) return PROVEN_ERR_INVALID_FORMAT;
        if (!terms && params.client_bits_seen) return PROVEN_ERR_INVALID_FORMAT;
        if (terms) {
            *terms = (proven_ws_deflate_terms_t){ .server_takeover = !params.server_no_takeover, .client_takeover = !params.client_no_takeover,
                                                  .server_window_bits = params.server_bits, .client_window_bits = params.client_bits };
        }
        *deflate = true;
    }
    proven_size_t protocols = proven_http_header_count(h, n, ws_lit("Sec-WebSocket-Protocol"));
    if (protocols > 1) return PROVEN_ERR_INVALID_FORMAT;
    if (protocols == 1) {
        proven_u8str_view_t p;
        if (!proven_http_header_find(h, n, ws_lit("Sec-WebSocket-Protocol"), &p)) return PROVEN_ERR_INVALID_FORMAT;
        p = ws_trim(p);
        if (p.size == 0 || !ws_list_has(offered, p)) return PROVEN_ERR_INVALID_FORMAT;
        *protocol = p;
    }
    return PROVEN_OK;
}

proven_err_t proven_ws_check_response(const proven_http_response_t *response, proven_u8str_view_t key,
                                      proven_u8str_view_t offered, proven_u8str_view_t *protocol) {
    return ws_check_response(response, key, offered, protocol, (void *)0, (void *)0, false);
}

proven_err_t proven_ws_check_response_deflate(const proven_http_response_t *response, proven_u8str_view_t key,
                                              proven_u8str_view_t offered, proven_u8str_view_t *protocol, bool *deflate) {
    if (!deflate) return PROVEN_ERR_INVALID_ARG;
    *deflate = false;
    return ws_check_response(response, key, offered, protocol, deflate, (void *)0, false);
}

proven_err_t proven_ws_check_response_takeover(const proven_http_response_t *response, proven_u8str_view_t key,
                                               proven_u8str_view_t offered, proven_u8str_view_t *protocol, bool *deflate,
                                               proven_ws_deflate_terms_t *terms) {
    if (!deflate || !terms) return PROVEN_ERR_INVALID_ARG;
    *deflate = false;
    *terms = (proven_ws_deflate_terms_t){ 0 };
    proven_err_t e = ws_check_response(response, key, offered, protocol, deflate, terms, false);
    if (e != PROVEN_OK) { *deflate = false; *terms = (proven_ws_deflate_terms_t){ 0 }; }
    return e;
}

proven_err_t proven_ws_check_response_peer_no_takeover(const proven_http_response_t *response, proven_u8str_view_t key,
                                                       proven_u8str_view_t offered, proven_u8str_view_t *protocol, bool *deflate,
                                                       proven_ws_deflate_terms_t *terms) {
    if (!deflate || !terms) return PROVEN_ERR_INVALID_ARG;
    *deflate = false;
    *terms = (proven_ws_deflate_terms_t){ 0 };
    proven_err_t e = ws_check_response(response, key, offered, protocol, deflate, terms, true);
    if (e != PROVEN_OK) { *deflate = false; *terms = (proven_ws_deflate_terms_t){ 0 }; }
    return e;
}

// -----------------------------------------------------------------------------
// Frames
// -----------------------------------------------------------------------------

static bool ws_opcode_known(proven_u8 op) {
    return op == PROVEN_WS_CONTINUATION || op == PROVEN_WS_TEXT || op == PROVEN_WS_BINARY ||
           op == PROVEN_WS_CLOSE || op == PROVEN_WS_PING || op == PROVEN_WS_PONG;
}

proven_err_t proven_ws_frame_parse(proven_mem_view_t data, proven_ws_frame_t *out, proven_size_t *header_size) {
    if (!out || !header_size || (data.size > 0 && !data.ptr)) return PROVEN_ERR_INVALID_ARG;
    if (data.size < 1) return PROVEN_ERR_NEED_MORE;
    proven_byte_t b0 = data.ptr[0];
    proven_u8 opcode = (proven_u8)(b0 & 0x0F);
    bool fin = (b0 & 0x80) != 0;
    bool control = (opcode & 0x08) != 0;
    /* The reserved bits belong to extensions. The first is permessage-deflate's, and means
     * something only where a message begins; no extension this library knows uses the others. */
    bool rsv1 = (b0 & 0x40) != 0;
    if ((b0 & 0x30) != 0 || !ws_opcode_known(opcode)) return PROVEN_ERR_INVALID_FORMAT;
    if (rsv1 && (control || opcode == PROVEN_WS_CONTINUATION)) return PROVEN_ERR_INVALID_FORMAT;
    if (control && !fin) return PROVEN_ERR_INVALID_FORMAT;
    if (data.size < 2) return PROVEN_ERR_NEED_MORE;
    proven_byte_t b1 = data.ptr[1];
    bool masked = (b1 & 0x80) != 0;
    proven_u64 length = (proven_u64)(b1 & 0x7F);
    proven_size_t pos = 2;
    if (control && length > PROVEN_WS_MAX_CONTROL) return PROVEN_ERR_INVALID_FORMAT;
    if (length == 126) {
        if (data.size < 4) return PROVEN_ERR_NEED_MORE;
        length = ((proven_u64)data.ptr[2] << 8) | (proven_u64)data.ptr[3];
        if (length < 126) return PROVEN_ERR_INVALID_FORMAT;         /* not the shortest form */
        pos = 4;
    } else if (length == 127) {
        if (data.size < 3) return PROVEN_ERR_NEED_MORE;
        if ((data.ptr[2] & 0x80) != 0) return PROVEN_ERR_INVALID_FORMAT;   /* the top bit must be 0 */
        if (data.size < 10) return PROVEN_ERR_NEED_MORE;
        length = 0;
        for (proven_size_t i = 0; i < 8; ++i) length = (length << 8) | (proven_u64)data.ptr[2 + i];
        if (length < 65536) return PROVEN_ERR_INVALID_FORMAT;
        pos = 10;
    }
    proven_ws_frame_t f = { .fin = fin, .opcode = opcode, .masked = masked, .length = length, .rsv1 = rsv1 };
    if (masked) {
        if (data.size < pos + 4) return PROVEN_ERR_NEED_MORE;
        for (proven_size_t i = 0; i < 4; ++i) f.mask[i] = data.ptr[pos + i];
        pos += 4;
    }
    *out = f;
    *header_size = pos;
    return PROVEN_OK;
}

proven_err_t proven_ws_frame_write(proven_mem_mut_t out, proven_size_t *len, const proven_ws_frame_t *frame) {
    if (!len || !frame || (out.size > 0 && !out.ptr) || *len > out.size) return PROVEN_ERR_INVALID_ARG;
    bool control = (frame->opcode & 0x08) != 0;
    if (!ws_opcode_known(frame->opcode) || (frame->length >> 63) != 0) return PROVEN_ERR_INVALID_ARG;
    if (control && (!frame->fin || frame->length > PROVEN_WS_MAX_CONTROL)) return PROVEN_ERR_INVALID_ARG;
    if (frame->rsv1 && (control || frame->opcode == PROVEN_WS_CONTINUATION)) return PROVEN_ERR_INVALID_ARG;
    proven_byte_t h[PROVEN_WS_MAX_FRAME_HEADER];
    proven_size_t n = 0;
    h[n++] = (proven_byte_t)((frame->fin ? 0x80 : 0) | (frame->rsv1 ? 0x40 : 0) | frame->opcode);
    proven_byte_t mask_bit = frame->masked ? 0x80 : 0;
    if (frame->length < 126) {
        h[n++] = (proven_byte_t)(mask_bit | (proven_byte_t)frame->length);
    } else if (frame->length < 65536) {
        h[n++] = (proven_byte_t)(mask_bit | 126);
        h[n++] = (proven_byte_t)(frame->length >> 8);
        h[n++] = (proven_byte_t)(frame->length & 0xFF);
    } else {
        h[n++] = (proven_byte_t)(mask_bit | 127);
        for (int shift = 56; shift >= 0; shift -= 8) h[n++] = (proven_byte_t)((frame->length >> shift) & 0xFF);
    }
    if (frame->masked) for (proven_size_t i = 0; i < 4; ++i) h[n++] = frame->mask[i];
    if (out.size - *len < n) return PROVEN_ERR_OUT_OF_BOUNDS;
    for (proven_size_t i = 0; i < n; ++i) out.ptr[*len + i] = h[i];
    *len += n;
    return PROVEN_OK;
}

void proven_ws_mask(proven_mem_mut_t data, const proven_byte_t mask[4], proven_u64 offset) {
    if (!data.ptr || !mask) return;
    for (proven_size_t i = 0; i < data.size; ++i) data.ptr[i] ^= mask[(offset + i) & 3];
}

// -----------------------------------------------------------------------------
// Closing
// -----------------------------------------------------------------------------

bool proven_ws_close_code_is_valid(proven_u16 code) {
    return (code >= 1000 && code <= 1003) || (code >= 1007 && code <= 1014) || (code >= 3000 && code <= 4999);
}

proven_err_t proven_ws_close_write(proven_mem_mut_t out, proven_size_t *len, proven_u16 code, proven_u8str_view_t reason) {
    if (!len || (out.size > 0 && !out.ptr) || *len > out.size || (reason.size > 0 && !reason.ptr)) return PROVEN_ERR_INVALID_ARG;
    if (code == PROVEN_WS_CLOSE_NO_STATUS) return reason.size == 0 ? PROVEN_OK : PROVEN_ERR_INVALID_ARG;
    if (!proven_ws_close_code_is_valid(code) || reason.size > PROVEN_WS_MAX_CONTROL - 2) return PROVEN_ERR_INVALID_ARG;
    if (!ws_utf8_ok(reason)) return PROVEN_ERR_INVALID_ENCODING;
    if (out.size - *len < 2 + reason.size) return PROVEN_ERR_OUT_OF_BOUNDS;
    out.ptr[(*len)++] = (proven_byte_t)(code >> 8);
    out.ptr[(*len)++] = (proven_byte_t)(code & 0xFF);
    for (proven_size_t i = 0; i < reason.size; ++i) out.ptr[(*len)++] = reason.ptr[i];
    return PROVEN_OK;
}

proven_err_t proven_ws_close_parse(proven_mem_view_t payload, proven_u16 *code, proven_u8str_view_t *reason) {
    if (!code || !reason || (payload.size > 0 && !payload.ptr)) return PROVEN_ERR_INVALID_ARG;
    *reason = (proven_u8str_view_t){0};
    *code = PROVEN_WS_CLOSE_NO_STATUS;
    if (payload.size == 0) return PROVEN_OK;
    if (payload.size == 1) return PROVEN_ERR_INVALID_FORMAT;
    proven_u16 c = (proven_u16)(((proven_u16)payload.ptr[0] << 8) | (proven_u16)payload.ptr[1]);
    if (!proven_ws_close_code_is_valid(c)) return PROVEN_ERR_INVALID_FORMAT;
    proven_u8str_view_t r = { .ptr = payload.ptr + 2, .size = payload.size - 2 };
    if (!ws_utf8_ok(r)) return PROVEN_ERR_INVALID_ENCODING;
    *code = c;
    *reason = r;
    return PROVEN_OK;
}

proven_u16 proven_ws_close_code_for(proven_err_t err) {
    if (err == PROVEN_ERR_INVALID_FORMAT) return PROVEN_WS_CLOSE_PROTOCOL_ERROR;
    if (err == PROVEN_ERR_INVALID_ENCODING) return PROVEN_WS_CLOSE_INVALID_DATA;
    if (err == PROVEN_ERR_OUT_OF_BOUNDS) return PROVEN_WS_CLOSE_TOO_BIG;
    return PROVEN_WS_CLOSE_INTERNAL_ERROR;
}

// -----------------------------------------------------------------------------
// The decoder
// -----------------------------------------------------------------------------

void proven_ws_decoder_init(proven_ws_decoder_t *decoder, bool from_client, proven_u64 max_message_bytes) {
    if (!decoder) return;
    *decoder = (proven_ws_decoder_t){0};
    decoder->expect_masked = from_client;
    decoder->max_message = max_message_bytes;
    decoder->failed = PROVEN_OK;
}

void proven_ws_decoder_allow_compressed(proven_ws_decoder_t *decoder) {
    if (decoder) decoder->allow_compressed = true;
}

/* Check a piece of text. A character cut by the end of the piece waits in `held` for the bytes
 * that complete it. (Internal: the drivers run it over what they inflate.) */
proven_err_t proven_ws_utf8_feed_(proven_byte_t held[4], proven_u8 *held_len, const proven_byte_t *p, proven_size_t n) {
    proven_size_t pos = 0;
    while (*held_len > 0 && pos < n) {
        held[(*held_len)++] = p[pos++];
        proven_utf8_char_t c = proven_utf8_decode_next((proven_u8str_view_t){ .ptr = held, .size = *held_len }, 0);
        if (c.err == PROVEN_OK) *held_len = 0;
        else if (c.err != PROVEN_ERR_NEED_MORE) return PROVEN_ERR_INVALID_ENCODING;
    }
    if (*held_len > 0) return PROVEN_OK;
    proven_u8str_view_t s = { .ptr = p, .size = n };
    while (pos < n) {
        if (p[pos] < 0x80) { pos++; continue; }
        proven_utf8_char_t c = proven_utf8_decode_next(s, pos);
        if (c.err == PROVEN_OK) { pos += c.len; continue; }
        if (c.err != PROVEN_ERR_NEED_MORE) return PROVEN_ERR_INVALID_ENCODING;
        while (pos < n) held[(*held_len)++] = p[pos++];        /* at most three bytes */
    }
    return PROVEN_OK;
}

static proven_err_t ws_text_feed(proven_ws_decoder_t *d, const proven_byte_t *p, proven_size_t n) {
    return proven_ws_utf8_feed_(d->utf8, &d->utf8_len, p, n);
}

proven_err_t proven_ws_decoder_feed(proven_ws_decoder_t *decoder, proven_mem_mut_t in, proven_size_t *consumed,
                                    proven_ws_event_t *event) {
    if (consumed) *consumed = 0;
    if (event) *event = (proven_ws_event_t){0};
    if (!decoder || !consumed || !event || (in.size > 0 && !in.ptr)) return PROVEN_ERR_INVALID_ARG;
    proven_ws_decoder_t *d = decoder;
    if (d->failed != PROVEN_OK) return d->failed;
    if (d->closed) return in.size > 0 ? PROVEN_ERR_EOF : PROVEN_OK;

    proven_size_t pos = 0;
    proven_err_t e = PROVEN_OK;
    for (;;) {
        if (!d->in_frame) {
            /* Gather the header a byte at a time: it is at most fourteen, and what follows it
             * must not be touched until its length is known. */
            bool have = false;
            while (pos < in.size && !have) {
                d->header[d->header_len++] = in.ptr[pos++];
                /* An extension nobody agreed to is refused at its first byte, as it always
                 * was - not when the rest of the header has arrived. */
                if (d->header_len == 1 && (d->header[0] & 0x40) != 0 && !d->allow_compressed) { e = PROVEN_ERR_INVALID_FORMAT; goto fail; }
                proven_size_t hs = 0;
                e = proven_ws_frame_parse((proven_mem_view_t){ .ptr = d->header, .size = d->header_len }, &d->frame, &hs);
                if (e == PROVEN_ERR_NEED_MORE) continue;
                if (e != PROVEN_OK) goto fail;
                have = true;
            }
            if (!have) { *consumed = pos; return PROVEN_OK; }
            d->header_len = 0;

            e = PROVEN_ERR_INVALID_FORMAT;
            if (d->frame.masked != d->expect_masked) goto fail;
            if (d->frame.rsv1 && !d->allow_compressed) goto fail;   /* an extension nobody agreed to */
            bool control = (d->frame.opcode & 0x08) != 0;
            if (control) {
                d->control_len = 0;
            } else {
                if (d->frame.opcode == PROVEN_WS_CONTINUATION) {
                    if (!d->in_message) goto fail;             /* nothing to continue */
                } else {
                    if (d->in_message) goto fail;              /* a new message inside an unfinished one */
                    d->in_message = true;
                    d->message_text = d->frame.opcode == PROVEN_WS_TEXT;
                    d->message_compressed = d->frame.rsv1;
                    d->message_started = false;
                    d->message_size = 0;
                    d->utf8_len = 0;
                }
                if (d->max_message != 0 && d->frame.length > d->max_message - d->message_size) {
                    e = PROVEN_ERR_OUT_OF_BOUNDS;
                    goto fail;
                }
            }
            d->in_frame = true;
            d->remaining = d->frame.length;
            d->offset = 0;
        }

        proven_size_t avail = in.size - pos;
        proven_size_t take = d->remaining < (proven_u64)avail ? (proven_size_t)d->remaining : avail;

        if ((d->frame.opcode & 0x08) != 0) {
            for (proven_size_t i = 0; i < take; ++i) d->control[d->control_len++] = in.ptr[pos + i];
            pos += take;
            d->remaining -= take;
            if (d->remaining > 0) { *consumed = pos; return PROVEN_OK; }
            if (d->frame.masked) proven_ws_mask((proven_mem_mut_t){ .ptr = d->control, .size = d->control_len }, d->frame.mask, 0);
            d->in_frame = false;
            proven_mem_view_t payload = { .ptr = d->control, .size = d->control_len };
            if (d->frame.opcode == PROVEN_WS_CLOSE) {
                proven_u8str_view_t reason;
                e = proven_ws_close_parse(payload, &event->close_code, &reason);
                if (e != PROVEN_OK) goto fail;
                event->kind = PROVEN_WS_EVENT_CLOSE;
                event->data = (proven_mem_view_t){ .ptr = reason.ptr, .size = reason.size };
                d->closed = true;
            } else {
                event->kind = d->frame.opcode == PROVEN_WS_PING ? PROVEN_WS_EVENT_PING : PROVEN_WS_EVENT_PONG;
                event->data = payload;
            }
            *consumed = pos;
            return PROVEN_OK;
        }

        if (take == 0 && d->remaining > 0) { *consumed = pos; return PROVEN_OK; }
        proven_byte_t *piece = in.ptr + pos;
        if (d->frame.masked) proven_ws_mask((proven_mem_mut_t){ .ptr = piece, .size = take }, d->frame.mask, d->offset);
        d->offset += take;
        d->remaining -= take;
        d->message_size += take;
        pos += take;
        /* Compressed bytes are not text yet: whoever inflates them checks what comes out. */
        bool check_text = d->message_text && !d->message_compressed;
        if (check_text) {
            e = ws_text_feed(d, piece, take);
            if (e != PROVEN_OK) goto fail;
        }
        bool frame_done = d->remaining == 0;
        bool last = frame_done && d->frame.fin;
        if (last && check_text && d->utf8_len != 0) { e = PROVEN_ERR_INVALID_ENCODING; goto fail; }
        if (frame_done) d->in_frame = false;
        if (last) d->in_message = false;
        if (take == 0 && !last) continue;          /* an empty frame in the middle of a message says nothing */
        event->kind = PROVEN_WS_EVENT_DATA;
        event->text = d->message_text;
        event->first = !d->message_started;
        event->last = last;
        event->compressed = d->message_compressed;
        event->data = (proven_mem_view_t){ .ptr = piece, .size = take };
        d->message_started = true;
        *consumed = pos;
        return PROVEN_OK;
    }

fail:
    d->failed = e;
    *consumed = pos;
    *event = (proven_ws_event_t){0};
    return e;
}
