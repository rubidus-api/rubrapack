#include "proven/http.h"

/*
 * An HTTP/1.1 message codec written from RFC 9110 and RFC 9112. Plain byte code: no C library
 * call, no allocation, no I/O - so this unit is part of the freestanding profile.
 *
 * The parsers validate as they scan and can stop at any byte. That gives the property the
 * callers rely on: a prefix of a valid head is always "need more", never an error and never a
 * success, so reading more is always the right response to it.
 */

// -----------------------------------------------------------------------------
// Characters
// -----------------------------------------------------------------------------

/* RFC 9110 section 5.6.2: tchar. */
static bool http_is_tchar(proven_byte_t c) {
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return true;
    switch (c) {
        case '!': case '#': case '$': case '%': case '&': case '\'': case '*': case '+':
        case '-': case '.': case '^': case '_': case '`': case '|': case '~':
            return true;
        default:
            return false;
    }
}

/* What may appear in a field value or a reason phrase: HTAB, SP, visible ASCII, and bytes
 * above ASCII (obs-text). Not NUL, not CR, not LF, not any other control. */
static bool http_is_field_byte(proven_byte_t c) {
    return c == '\t' || (c >= 0x20 && c != 0x7f);
}

static bool http_is_ows(proven_byte_t c) { return c == ' ' || c == '\t'; }

static proven_byte_t http_lower(proven_byte_t c) { return (c >= 'A' && c <= 'Z') ? (proven_byte_t)(c + 32) : c; }


static bool http_eq(proven_u8str_view_t a, proven_u8str_view_t b) {
    if (a.size != b.size) return false;
    for (proven_size_t i = 0; i < a.size; ++i) if (a.ptr[i] != b.ptr[i]) return false;
    return true;
}

static bool http_is_token(proven_u8str_view_t s) {
    if (s.size == 0) return false;
    for (proven_size_t i = 0; i < s.size; ++i) if (!http_is_tchar(s.ptr[i])) return false;
    return true;
}

// -----------------------------------------------------------------------------
// The head scanner
// -----------------------------------------------------------------------------

typedef struct {
    const proven_byte_t *p;
    proven_size_t n;        /* bytes that may be looked at: min(data.size, max_head) */
    bool capped;            /* n was cut by the limit: running out means "too large" */
    proven_size_t pos;
} proven_http_cur_t;

/* The scan ran out of bytes. Either more will come, or the limit has been reached with no end
 * of head in sight - decided here so that an endless header costs the limit and no more. */
static proven_err_t http_short(const proven_http_cur_t *c) {
    return c->capped ? PROVEN_ERR_OUT_OF_BOUNDS : PROVEN_ERR_NEED_MORE;
}

static proven_http_cur_t http_cur(proven_mem_view_t data, proven_size_t max_head_bytes) {
    proven_size_t limit = max_head_bytes ? max_head_bytes : PROVEN_HTTP_DEFAULT_MAX_HEAD;
    proven_http_cur_t c = { .p = data.ptr, .n = data.size, .capped = false, .pos = 0 };
    if (c.n >= limit) { c.n = limit; c.capped = true; }
    return c;
}

/* Match a literal. A mismatch is an error even when the input ends first, as long as what is
 * there already disagrees; agreement followed by the end of input is "need more". */
static proven_err_t http_expect(proven_http_cur_t *c, const char *lit) {
    for (proven_size_t i = 0; lit[i] != '\0'; ++i) {
        if (c->pos >= c->n) return http_short(c);
        if (c->p[c->pos] != (proven_byte_t)lit[i]) return PROVEN_ERR_INVALID_FORMAT;
        c->pos++;
    }
    return PROVEN_OK;
}

/* "1.0" or "1.1" after "HTTP/". Any other version made of digits is a version this codec does
 * not speak; anything else is not a version. */
static proven_err_t http_version(proven_http_cur_t *c, proven_u8 *minor) {
    proven_err_t e = http_expect(c, "HTTP/");
    if (e != PROVEN_OK) return e;
    if (c->pos >= c->n) return http_short(c);
    proven_byte_t major = c->p[c->pos];
    if (major < '0' || major > '9') return PROVEN_ERR_INVALID_FORMAT;
    c->pos++;
    if (c->pos >= c->n) return http_short(c);
    if (c->p[c->pos] != '.') return PROVEN_ERR_INVALID_FORMAT;
    c->pos++;
    if (c->pos >= c->n) return http_short(c);
    proven_byte_t mn = c->p[c->pos];
    if (mn < '0' || mn > '9') return PROVEN_ERR_INVALID_FORMAT;
    c->pos++;
    if (major != '1' || (mn != '0' && mn != '1')) return PROVEN_ERR_UNSUPPORTED;
    *minor = (proven_u8)(mn - '0');
    return PROVEN_OK;
}

/* Header fields up to and including the empty line. */
/* `trailers`: leave out the fields that may not be trailers (RFC 9110 section 6.5.1), so that one
 * received is dropped rather than taken for what the head said. */
static proven_err_t http_fields_of(proven_http_cur_t *c, proven_http_header_t *headers, proven_size_t cap, proven_size_t *count, bool trailers) {
    *count = 0;
    for (;;) {
        if (c->pos >= c->n) return http_short(c);
        proven_byte_t first = c->p[c->pos];
        if (first == '\r') {
            c->pos++;
            if (c->pos >= c->n) return http_short(c);
            if (c->p[c->pos] != '\n') return PROVEN_ERR_INVALID_FORMAT;
            c->pos++;
            return PROVEN_OK;
        }
        /* A line that starts with whitespace is the obsolete folding of the previous value
         * onto a second line. Parsers disagree about what it means, which is the problem. */
        proven_size_t name_start = c->pos;
        while (c->pos < c->n && http_is_tchar(c->p[c->pos])) c->pos++;
        if (c->pos >= c->n) return http_short(c);
        /* The name ends at ':' and nowhere else: "Name : value" is refused, not repaired. */
        if (c->p[c->pos] != ':' || c->pos == name_start) return PROVEN_ERR_INVALID_FORMAT;
        proven_size_t name_end = c->pos;
        c->pos++;

        while (c->pos < c->n && http_is_ows(c->p[c->pos])) c->pos++;
        proven_size_t value_start = c->pos;
        while (c->pos < c->n && http_is_field_byte(c->p[c->pos])) c->pos++;
        if (c->pos >= c->n) return http_short(c);
        if (c->p[c->pos] != '\r') return PROVEN_ERR_INVALID_FORMAT;      /* NUL, a bare LF, a control */
        proven_size_t value_end = c->pos;
        c->pos++;
        if (c->pos >= c->n) return http_short(c);
        if (c->p[c->pos] != '\n') return PROVEN_ERR_INVALID_FORMAT;      /* a bare CR */
        c->pos++;
        while (value_end > value_start && http_is_ows(c->p[value_end - 1])) value_end--;

        if (trailers && !proven_http_trailer_allowed((proven_u8str_view_t){ .ptr = c->p + name_start, .size = name_end - name_start })) continue;
        if (*count >= cap) return PROVEN_ERR_OUT_OF_BOUNDS;
        headers[*count].name = (proven_u8str_view_t){ .ptr = c->p + name_start, .size = name_end - name_start };
        headers[*count].value = (proven_u8str_view_t){ .ptr = c->p + value_start, .size = value_end - value_start };
        (*count)++;
    }
}

static proven_err_t http_fields(proven_http_cur_t *c, proven_http_header_t *headers, proven_size_t cap, proven_size_t *count) {
    return http_fields_of(c, headers, cap, count, false);
}

proven_err_t proven_http_parse_request(proven_mem_view_t data, proven_http_header_t *headers, proven_size_t header_cap,
                                       proven_size_t max_head_bytes, proven_http_request_t *out, proven_size_t *head_size) {
    if (!out || !head_size || (data.size > 0 && !data.ptr) || (header_cap > 0 && !headers)) return PROVEN_ERR_INVALID_ARG;
    proven_http_cur_t c = http_cur(data, max_head_bytes);
    proven_http_request_t req = {0};

    /* Empty lines before the request line are skipped: clients have sent a stray CRLF after a
     * body for decades, and RFC 9112 section 2.2 says to ignore it. They count toward the
     * limit, and they cannot hide anything - a request line follows or nothing does. */
    for (;;) {
        if (c.pos >= c.n) return http_short(&c);
        if (c.p[c.pos] != '\r') break;
        c.pos++;
        if (c.pos >= c.n) return http_short(&c);
        if (c.p[c.pos] != '\n') return PROVEN_ERR_INVALID_FORMAT;
        c.pos++;
    }

    proven_size_t start = c.pos;
    while (c.pos < c.n && http_is_tchar(c.p[c.pos])) c.pos++;
    if (c.pos >= c.n) return http_short(&c);
    if (c.p[c.pos] != ' ' || c.pos == start) return PROVEN_ERR_INVALID_FORMAT;
    req.method_text = (proven_u8str_view_t){ .ptr = c.p + start, .size = c.pos - start };
    req.method = proven_http_method_from_text(req.method_text);
    c.pos++;

    /* The target: visible ASCII with no space in it. Exactly one space on each side; a second
     * one would be an empty target or a fourth field, and both are refused. */
    start = c.pos;
    while (c.pos < c.n && c.p[c.pos] > 0x20 && c.p[c.pos] < 0x7f) c.pos++;
    if (c.pos >= c.n) return http_short(&c);
    if (c.p[c.pos] != ' ' || c.pos == start) return PROVEN_ERR_INVALID_FORMAT;
    req.target = (proven_u8str_view_t){ .ptr = c.p + start, .size = c.pos - start };
    c.pos++;

    proven_err_t e = http_version(&c, &req.version_minor);
    if (e != PROVEN_OK) return e;
    e = http_expect(&c, "\r\n");
    if (e != PROVEN_OK) return e;

    e = http_fields(&c, headers, header_cap, &req.header_count);
    if (e != PROVEN_OK) return e;
    req.headers = headers;
    *out = req;
    *head_size = c.pos;
    return PROVEN_OK;
}

proven_err_t proven_http_parse_response(proven_mem_view_t data, proven_http_header_t *headers, proven_size_t header_cap,
                                        proven_size_t max_head_bytes, proven_http_response_t *out, proven_size_t *head_size) {
    if (!out || !head_size || (data.size > 0 && !data.ptr) || (header_cap > 0 && !headers)) return PROVEN_ERR_INVALID_ARG;
    proven_http_cur_t c = http_cur(data, max_head_bytes);
    proven_http_response_t res = {0};

    proven_err_t e = http_version(&c, &res.version_minor);
    if (e != PROVEN_OK) return e;
    e = http_expect(&c, " ");
    if (e != PROVEN_OK) return e;

    proven_u32 status = 0;
    for (int i = 0; i < 3; ++i) {
        if (c.pos >= c.n) return http_short(&c);
        proven_byte_t d = c.p[c.pos];
        if (d < '0' || d > '9') return PROVEN_ERR_INVALID_FORMAT;
        status = status * 10u + (proven_u32)(d - '0');
        c.pos++;
    }
    if (status < 100u) return PROVEN_ERR_INVALID_FORMAT;
    res.status = (proven_u16)status;

    /* "HTTP/1.1 200 OK", and also "HTTP/1.1 200" with no phrase and no space before the line
     * ends: servers send it, and nothing can be smuggled in a phrase that is not there. */
    if (c.pos >= c.n) return http_short(&c);
    if (c.p[c.pos] == ' ') {
        c.pos++;
        proven_size_t start = c.pos;
        while (c.pos < c.n && http_is_field_byte(c.p[c.pos])) c.pos++;
        if (c.pos >= c.n) return http_short(&c);
        res.reason = (proven_u8str_view_t){ .ptr = c.p + start, .size = c.pos - start };
    } else {
        res.reason = (proven_u8str_view_t){ .ptr = c.p + c.pos, .size = 0 };
    }
    e = http_expect(&c, "\r\n");
    if (e != PROVEN_OK) return e;

    e = http_fields(&c, headers, header_cap, &res.header_count);
    if (e != PROVEN_OK) return e;
    res.headers = headers;
    *out = res;
    *head_size = c.pos;
    return PROVEN_OK;
}

// -----------------------------------------------------------------------------
// Looking at a head
// -----------------------------------------------------------------------------

bool proven_http_header_find(const proven_http_header_t *headers, proven_size_t count,
                             proven_u8str_view_t name, proven_u8str_view_t *value) {
    if (!headers) return false;
    for (proven_size_t i = 0; i < count; ++i) {
        if (proven_u8str_view_eq_ascii_ci(headers[i].name, name)) {
            if (value) *value = headers[i].value;
            return true;
        }
    }
    return false;
}

proven_size_t proven_http_header_count(const proven_http_header_t *headers, proven_size_t count, proven_u8str_view_t name) {
    proven_size_t n = 0;
    if (!headers) return 0;
    for (proven_size_t i = 0; i < count; ++i) if (proven_u8str_view_eq_ascii_ci(headers[i].name, name)) n++;
    return n;
}

/* Walk a comma-separated list: the next element with its surrounding whitespace removed. */
static bool http_list_next(proven_u8str_view_t *rest, proven_u8str_view_t *item) {
    if (rest->size == 0) return false;
    proven_size_t end = 0;
    while (end < rest->size && rest->ptr[end] != ',') end++;
    proven_size_t a = 0, b = end;
    while (a < b && http_is_ows(rest->ptr[a])) a++;
    while (b > a && http_is_ows(rest->ptr[b - 1])) b--;
    *item = (proven_u8str_view_t){ .ptr = rest->ptr + a, .size = b - a };
    proven_size_t skip = end < rest->size ? end + 1 : end;
    *rest = (proven_u8str_view_t){ .ptr = rest->ptr + skip, .size = rest->size - skip };
    /* "a," has an empty element after the comma that the loop above would not visit; it is
     * harmless to ignore, as RFC 9110 section 5.6.1 tells recipients to. */
    return true;
}

bool proven_http_header_has_token(const proven_http_header_t *headers, proven_size_t count,
                                  proven_u8str_view_t name, proven_u8str_view_t token) {
    if (!headers) return false;
    for (proven_size_t i = 0; i < count; ++i) {
        if (!proven_u8str_view_eq_ascii_ci(headers[i].name, name)) continue;
        proven_u8str_view_t rest = headers[i].value, item;
        while (http_list_next(&rest, &item)) {
            if (proven_u8str_view_eq_ascii_ci(item, token)) return true;
        }
    }
    return false;
}

static proven_u8str_view_t http_lit_n(const char *s, proven_size_t n) {
    return (proven_u8str_view_t){ .ptr = (const proven_byte_t *)s, .size = n };
}
#define HTTP_LIT(s) http_lit_n("" s, sizeof(s) - 1)

static proven_http_coding_t http_coding_named(proven_u8str_view_t name) {
    if (proven_u8str_view_eq_ascii_ci(name, HTTP_LIT("gzip")) || proven_u8str_view_eq_ascii_ci(name, HTTP_LIT("x-gzip"))) return PROVEN_HTTP_CODING_GZIP;
    if (proven_u8str_view_eq_ascii_ci(name, HTTP_LIT("deflate"))) return PROVEN_HTTP_CODING_DEFLATE;
    if (proven_u8str_view_eq_ascii_ci(name, HTTP_LIT("zstd"))) return PROVEN_HTTP_CODING_ZSTD;
    if (proven_u8str_view_eq_ascii_ci(name, HTTP_LIT("br"))) return PROVEN_HTTP_CODING_BROTLI;
    if (proven_u8str_view_eq_ascii_ci(name, HTTP_LIT("identity"))) return PROVEN_HTTP_CODING_IDENTITY;
    return PROVEN_HTTP_CODING_OTHER;
}

proven_http_coding_t proven_http_content_coding(const proven_http_header_t *headers, proven_size_t count) {
    proven_http_coding_t found = PROVEN_HTTP_CODING_IDENTITY;
    proven_size_t fields = 0, items = 0;
    if (!headers) return found;
    for (proven_size_t i = 0; i < count; ++i) {
        if (!proven_u8str_view_eq_ascii_ci(headers[i].name, HTTP_LIT("Content-Encoding"))) continue;
        if (++fields > 1) return PROVEN_HTTP_CODING_OTHER;
        proven_u8str_view_t rest = headers[i].value, item;
        while (http_list_next(&rest, &item)) {
            if (item.size == 0) continue;
            if (++items > 1) return PROVEN_HTTP_CODING_OTHER;
            found = http_coding_named(item);
        }
    }
    return found;
}

/* One element of Accept-Encoding: `coding [ OWS ";" OWS "q=" qvalue ]`. The weight comes back
 * in thousandths. */
static bool http_accept_item(proven_u8str_view_t item, proven_u8str_view_t *name, unsigned *weight) {
    proven_size_t n = 0;
    while (n < item.size && item.ptr[n] != ';' && item.ptr[n] != ' ' && item.ptr[n] != '\t') n++;
    if (n == 0) return false;
    *name = (proven_u8str_view_t){ .ptr = item.ptr, .size = n };
    *weight = 1000;
    proven_size_t i = n;
    while (i < item.size && (item.ptr[i] == ' ' || item.ptr[i] == '\t')) i++;
    if (i == item.size) return true;
    if (item.ptr[i++] != ';') return false;
    while (i < item.size && (item.ptr[i] == ' ' || item.ptr[i] == '\t')) i++;
    if (item.size - i < 3 || (item.ptr[i] != 'q' && item.ptr[i] != 'Q') || item.ptr[i + 1] != '=') return false;
    i += 2;
    proven_byte_t lead = item.ptr[i++];
    if (lead != '0' && lead != '1') return false;
    unsigned w = lead == '1' ? 1000u : 0u, scale = 100;
    if (i < item.size) {
        if (item.ptr[i++] != '.') return false;
        if (item.size - i > 3) return false;
        for (; i < item.size; ++i, scale /= 10) {
            if (item.ptr[i] < '0' || item.ptr[i] > '9') return false;
            w += (unsigned)(item.ptr[i] - '0') * scale;
        }
    }
    if (w > 1000) return false;
    *weight = w;
    return true;
}

unsigned proven_http_accept_weight(const proven_http_header_t *headers, proven_size_t count, proven_http_coding_t coding) {
    if (coding == PROVEN_HTTP_CODING_OTHER) return 0;
    const unsigned identity = coding == PROVEN_HTTP_CODING_IDENTITY ? 1000u : 0u;
    bool listed = false, starred = false;
    unsigned listed_weight = 0, star_weight = 0;
    if (!headers) return identity;
    for (proven_size_t i = 0; i < count; ++i) {
        if (!proven_u8str_view_eq_ascii_ci(headers[i].name, HTTP_LIT("Accept-Encoding"))) continue;
        proven_u8str_view_t rest = headers[i].value, item;
        while (http_list_next(&rest, &item)) {
            proven_u8str_view_t name;
            unsigned weight = 0;
            if (item.size == 0) continue;
            if (!http_accept_item(item, &name, &weight)) return identity;
            if (name.size == 1 && name.ptr[0] == '*') {
                if (!starred) { starred = true; star_weight = weight; }
            } else if (http_coding_named(name) == coding && !listed) {
                listed = true;
                listed_weight = weight;
            }
        }
    }
    if (listed) return listed_weight;
    if (starred) return star_weight;
    return identity;
}

bool proven_http_accepts_coding(const proven_http_header_t *headers, proven_size_t count, proven_http_coding_t coding) {
    return proven_http_accept_weight(headers, count, coding) > 0;
}

static const struct { proven_http_method_t method; const char *text; } http_methods[] = {
    { PROVEN_HTTP_GET, "GET" }, { PROVEN_HTTP_HEAD, "HEAD" }, { PROVEN_HTTP_POST, "POST" },
    { PROVEN_HTTP_PUT, "PUT" }, { PROVEN_HTTP_DELETE, "DELETE" }, { PROVEN_HTTP_CONNECT, "CONNECT" },
    { PROVEN_HTTP_OPTIONS, "OPTIONS" }, { PROVEN_HTTP_TRACE, "TRACE" }, { PROVEN_HTTP_PATCH, "PATCH" },
};

static proven_u8str_view_t http_cstr(const char *s) {
    proven_size_t n = 0;
    while (s[n] != '\0') n++;
    return (proven_u8str_view_t){ .ptr = (const proven_byte_t *)s, .size = n };
}

proven_http_method_t proven_http_method_from_text(proven_u8str_view_t text) {
    for (proven_size_t i = 0; i < sizeof http_methods / sizeof http_methods[0]; ++i) {
        if (http_eq(text, http_cstr(http_methods[i].text))) return http_methods[i].method;
    }
    return PROVEN_HTTP_METHOD_OTHER;
}

proven_u8str_view_t proven_http_method_text(proven_http_method_t method) {
    for (proven_size_t i = 0; i < sizeof http_methods / sizeof http_methods[0]; ++i) {
        if (http_methods[i].method == method) return http_cstr(http_methods[i].text);
    }
    return (proven_u8str_view_t){ .ptr = (const proven_byte_t *)"", .size = 0 };
}

proven_u8str_view_t proven_http_reason_phrase(proven_u16 status) {
    static const struct { proven_u16 status; const char *text; } phrases[] = {
        { 100, "Continue" }, { 101, "Switching Protocols" },
        { 200, "OK" }, { 201, "Created" }, { 202, "Accepted" }, { 203, "Non-Authoritative Information" },
        { 204, "No Content" }, { 205, "Reset Content" }, { 206, "Partial Content" },
        { 300, "Multiple Choices" }, { 301, "Moved Permanently" }, { 302, "Found" }, { 303, "See Other" },
        { 304, "Not Modified" }, { 307, "Temporary Redirect" }, { 308, "Permanent Redirect" },
        { 400, "Bad Request" }, { 401, "Unauthorized" }, { 403, "Forbidden" }, { 404, "Not Found" },
        { 405, "Method Not Allowed" }, { 406, "Not Acceptable" }, { 407, "Proxy Authentication Required" },
        { 408, "Request Timeout" }, { 409, "Conflict" }, { 410, "Gone" }, { 411, "Length Required" },
        { 412, "Precondition Failed" }, { 413, "Content Too Large" }, { 414, "URI Too Long" },
        { 415, "Unsupported Media Type" }, { 416, "Range Not Satisfiable" }, { 417, "Expectation Failed" },
        { 421, "Misdirected Request" }, { 422, "Unprocessable Content" }, { 426, "Upgrade Required" },
        { 428, "Precondition Required" }, { 429, "Too Many Requests" }, { 431, "Request Header Fields Too Large" },
        { 500, "Internal Server Error" }, { 501, "Not Implemented" }, { 502, "Bad Gateway" },
        { 503, "Service Unavailable" }, { 504, "Gateway Timeout" }, { 505, "HTTP Version Not Supported" },
    };
    for (proven_size_t i = 0; i < sizeof phrases / sizeof phrases[0]; ++i) {
        if (phrases[i].status == status) return http_cstr(phrases[i].text);
    }
    return http_cstr("Unknown");
}

static bool http_keep_alive(const proven_http_header_t *headers, proven_size_t count, proven_u8 minor) {
    proven_u8str_view_t connection = http_cstr("Connection");
    if (proven_http_header_has_token(headers, count, connection, http_cstr("close"))) return false;
    if (minor >= 1) return true;
    return proven_http_header_has_token(headers, count, connection, http_cstr("keep-alive"));
}

bool proven_http_request_keep_alive(const proven_http_request_t *request) {
    return request && http_keep_alive(request->headers, request->header_count, request->version_minor);
}

bool proven_http_response_keep_alive(const proven_http_response_t *response) {
    return response && http_keep_alive(response->headers, response->header_count, response->version_minor);
}

// -----------------------------------------------------------------------------
// Framing
// -----------------------------------------------------------------------------

/* The framing headers of one head. OK with `*out` filled, or the reason it is unacceptable. */
static proven_err_t http_framing_headers(const proven_http_header_t *headers, proven_size_t count, proven_u8 minor,
                                         bool *has_framing, proven_http_framing_t *out) {
    proven_u8str_view_t te_name = http_cstr("Transfer-Encoding");
    proven_u8str_view_t cl_name = http_cstr("Content-Length");
    proven_size_t te = proven_http_header_count(headers, count, te_name);
    proven_size_t cl = proven_http_header_count(headers, count, cl_name);
    *has_framing = false;

    /* Both: the two say different things about where the body ends, and a proxy and a server
     * that each believe a different one are how a request is smuggled. */
    if (te > 0 && cl > 0) return PROVEN_ERR_INVALID_FORMAT;

    if (te > 0) {
        if (minor == 0) return PROVEN_ERR_INVALID_FORMAT;    /* HTTP/1.0 has no transfer codings */
        proven_size_t codings = 0;
        bool only_chunked = true;
        for (proven_size_t i = 0; i < count; ++i) {
            if (!proven_u8str_view_eq_ascii_ci(headers[i].name, te_name)) continue;
            proven_u8str_view_t rest = headers[i].value, item;
            while (http_list_next(&rest, &item)) {
                if (item.size == 0) continue;
                /* A coding is a token, possibly with parameters; "chunked" has none. */
                proven_size_t t = 0;
                while (t < item.size && http_is_tchar(item.ptr[t])) t++;
                if (t == 0) return PROVEN_ERR_INVALID_FORMAT;
                codings++;
                if (t != item.size || !proven_u8str_view_eq_ascii_ci(item, http_cstr("chunked"))) only_chunked = false;
            }
        }
        if (codings == 0) return PROVEN_ERR_INVALID_FORMAT;
        if (codings != 1 || !only_chunked) return PROVEN_ERR_UNSUPPORTED;
        out->kind = PROVEN_HTTP_BODY_CHUNKED;
        out->length = 0;
        *has_framing = true;
        return PROVEN_OK;
    }

    if (cl > 0) {
        /* One field, digits only. Two fields are refused even when they agree; so is a list
         * ("42, 42"), a sign, and whitespace inside the number. */
        if (cl != 1) return PROVEN_ERR_INVALID_FORMAT;
        proven_u8str_view_t v = {0};
        (void)proven_http_header_find(headers, count, cl_name, &v);
        if (v.size == 0) return PROVEN_ERR_INVALID_FORMAT;
        proven_u64 n = 0;
        for (proven_size_t i = 0; i < v.size; ++i) {
            proven_byte_t d = v.ptr[i];
            if (d < '0' || d > '9') return PROVEN_ERR_INVALID_FORMAT;
            proven_u64 digit = (proven_u64)(d - '0');
            if (n > (UINT64_MAX - digit) / 10u) return PROVEN_ERR_INVALID_FORMAT;
            n = n * 10u + digit;
        }
        out->kind = PROVEN_HTTP_BODY_LENGTH;
        out->length = n;
        *has_framing = true;
        return PROVEN_OK;
    }
    return PROVEN_OK;
}

proven_err_t proven_http_request_framing(const proven_http_request_t *request, proven_http_framing_t *out) {
    if (!request || !out) return PROVEN_ERR_INVALID_ARG;
    proven_http_framing_t f = { .kind = PROVEN_HTTP_BODY_NONE, .length = 0 };
    bool has = false;
    proven_err_t e = http_framing_headers(request->headers, request->header_count, request->version_minor, &has, &f);
    if (e != PROVEN_OK) return e;
    *out = f;
    return PROVEN_OK;
}

proven_err_t proven_http_response_framing(const proven_http_response_t *response, proven_http_method_t request_method,
                                          proven_http_framing_t *out) {
    if (!response || !out) return PROVEN_ERR_INVALID_ARG;
    proven_http_framing_t f = { .kind = PROVEN_HTTP_BODY_NONE, .length = 0 };
    bool has = false;
    /* The headers are checked even when the status says there is no body: a response that
     * carries contradictory framing is malformed whatever its status. */
    proven_err_t e = http_framing_headers(response->headers, response->header_count, response->version_minor, &has, &f);
    if (e != PROVEN_OK) return e;

    proven_u16 s = response->status;
    bool bodiless = request_method == PROVEN_HTTP_HEAD || (s >= 100 && s < 200) || s == 204 || s == 304 ||
                    (request_method == PROVEN_HTTP_CONNECT && s >= 200 && s < 300);
    if (bodiless) {
        out->kind = PROVEN_HTTP_BODY_NONE;
        out->length = 0;
        return PROVEN_OK;
    }
    if (!has) f.kind = PROVEN_HTTP_BODY_UNTIL_CLOSE;
    *out = f;
    return PROVEN_OK;
}

// -----------------------------------------------------------------------------
// The body decoder
// -----------------------------------------------------------------------------

enum {
    HTTP_B_DATA = 0,        /* inside a fixed-length body, an until-close body, or a chunk */
    HTTP_B_SIZE,            /* reading the hex digits of a chunk size */
    HTTP_B_EXT,             /* skipping chunk extensions up to CR */
    HTTP_B_SIZE_LF,
    HTTP_B_DATA_CR,         /* the CRLF after a chunk's data */
    HTTP_B_DATA_LF,
    HTTP_B_TRAILER_START,   /* at the start of a trailer line, or of the final empty line */
    HTTP_B_TRAILER_NAME,
    HTTP_B_TRAILER_VALUE,
    HTTP_B_TRAILER_LF,
    HTTP_B_FINAL_LF,
    HTTP_B_FAILED
};

proven_err_t proven_http_body_init(proven_http_body_t *body, proven_http_framing_t framing, proven_u64 max_body_bytes) {
    if (!body) return PROVEN_ERR_INVALID_ARG;
    *body = (proven_http_body_t){0};
    body->max_body = max_body_bytes;
    switch (framing.kind) {
        case PROVEN_HTTP_BODY_NONE:
            body->kind = PROVEN_HTTP_BODY_NONE;
            body->done = true;
            return PROVEN_OK;
        case PROVEN_HTTP_BODY_LENGTH:
            if (framing.length > max_body_bytes) { body->state = HTTP_B_FAILED; return PROVEN_ERR_OUT_OF_BOUNDS; }
            body->kind = PROVEN_HTTP_BODY_LENGTH;
            body->remaining = framing.length;
            body->done = framing.length == 0;
            return PROVEN_OK;
        case PROVEN_HTTP_BODY_CHUNKED:
            body->kind = PROVEN_HTTP_BODY_CHUNKED;
            body->state = HTTP_B_SIZE;
            return PROVEN_OK;
        case PROVEN_HTTP_BODY_UNTIL_CLOSE:
            body->kind = PROVEN_HTTP_BODY_UNTIL_CLOSE;
            return PROVEN_OK;
        default:
            body->state = HTTP_B_FAILED;
            return PROVEN_ERR_INVALID_ARG;
    }
}

static int http_hex(proven_byte_t c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static proven_err_t http_body_fail(proven_http_body_t *body, proven_err_t e) {
    body->state = HTTP_B_FAILED;
    return e;
}

proven_err_t proven_http_body_feed(proven_http_body_t *body, proven_mem_view_t in,
                                   proven_size_t *consumed, proven_mem_view_t *payload, bool *done) {
    if (consumed) *consumed = 0;
    if (payload) *payload = (proven_mem_view_t){ .ptr = in.ptr, .size = 0 };
    if (done) *done = false;
    if (!body || !consumed || !payload || !done || (in.size > 0 && !in.ptr)) return PROVEN_ERR_INVALID_ARG;
    if (body->state == HTTP_B_FAILED) return PROVEN_ERR_INVALID_STATE;
    if (body->done) { *done = true; return PROVEN_OK; }

    proven_size_t i = 0;

    if (body->kind == PROVEN_HTTP_BODY_UNTIL_CLOSE) {
        if ((proven_u64)in.size > body->max_body - body->total) return http_body_fail(body, PROVEN_ERR_OUT_OF_BOUNDS);
        body->total += in.size;
        *payload = in;
        *consumed = in.size;
        return PROVEN_OK;
    }

    while (i < in.size) {
        proven_byte_t c = in.ptr[i];
        if (body->trailers && body->state >= HTTP_B_TRAILER_START && body->state <= HTTP_B_FINAL_LF) {
            /* The trailer section, kept as it comes - its last CRLF too, so that it reads as a head's fields. */
            if (body->line_bytes >= body->trailer_cap) return http_body_fail(body, PROVEN_ERR_OUT_OF_BOUNDS);
            body->trailers[body->line_bytes] = c;    /* line_bytes counts the section's bytes so far */
        }
        switch (body->state) {
            case HTTP_B_DATA: {
                proven_size_t take = in.size - i;
                if ((proven_u64)take > body->remaining) take = (proven_size_t)body->remaining;
                payload->ptr = in.ptr + i;
                payload->size = take;
                body->remaining -= take;
                body->total += take;
                i += take;
                if (body->remaining == 0) {
                    if (body->kind == PROVEN_HTTP_BODY_LENGTH) { body->done = true; *done = true; }
                    else body->state = HTTP_B_DATA_CR;
                }
                /* Hand the bytes back now: a payload is one contiguous view, and the next
                 * chunk's data is separated from this one by framing. */
                *consumed = i;
                return PROVEN_OK;
            }
            case HTTP_B_SIZE: {
                int d = http_hex(c);
                if (d >= 0) {
                    /* Sixteen hex digits fill 64 bits; a seventeenth cannot be a size. */
                    if (body->size_digits >= 16) return http_body_fail(body, PROVEN_ERR_INVALID_FORMAT);
                    body->remaining = (body->remaining << 4) | (proven_u64)d;
                    body->size_digits++;
                } else if (body->size_digits == 0) {
                    return http_body_fail(body, PROVEN_ERR_INVALID_FORMAT);
                } else if (c == ';') {
                    body->state = HTTP_B_EXT;
                } else if (c == '\r') {
                    body->state = HTTP_B_SIZE_LF;
                } else {
                    /* Whitespace after the size is where two chunk parsers part ways. */
                    return http_body_fail(body, PROVEN_ERR_INVALID_FORMAT);
                }
                break;
            }
            case HTTP_B_EXT:
                if (c == '\r') body->state = HTTP_B_SIZE_LF;
                else if (!http_is_field_byte(c)) return http_body_fail(body, PROVEN_ERR_INVALID_FORMAT);
                break;
            case HTTP_B_SIZE_LF:
                if (c != '\n') return http_body_fail(body, PROVEN_ERR_INVALID_FORMAT);
                if (body->remaining == 0) {
                    body->state = HTTP_B_TRAILER_START;
                    body->line_bytes = 0;
                    i++;
                    continue;
                }
                if (body->remaining > body->max_body - body->total) return http_body_fail(body, PROVEN_ERR_OUT_OF_BOUNDS);
                body->state = HTTP_B_DATA;
                body->line_bytes = 0;
                i++;
                continue;
            case HTTP_B_DATA_CR:
                if (c != '\r') return http_body_fail(body, PROVEN_ERR_INVALID_FORMAT);
                body->state = HTTP_B_DATA_LF;
                break;
            case HTTP_B_DATA_LF:
                if (c != '\n') return http_body_fail(body, PROVEN_ERR_INVALID_FORMAT);
                body->state = HTTP_B_SIZE;
                body->size_digits = 0;
                body->remaining = 0;
                body->line_bytes = 0;
                i++;
                continue;
            case HTTP_B_TRAILER_START:
                if (c == '\r') body->state = HTTP_B_FINAL_LF;
                else if (http_is_tchar(c)) body->state = HTTP_B_TRAILER_NAME;
                else return http_body_fail(body, PROVEN_ERR_INVALID_FORMAT);
                break;
            case HTTP_B_TRAILER_NAME:
                if (c == ':') body->state = HTTP_B_TRAILER_VALUE;
                else if (!http_is_tchar(c)) return http_body_fail(body, PROVEN_ERR_INVALID_FORMAT);
                break;
            case HTTP_B_TRAILER_VALUE:
                if (c == '\r') body->state = HTTP_B_TRAILER_LF;
                else if (!http_is_field_byte(c)) return http_body_fail(body, PROVEN_ERR_INVALID_FORMAT);
                break;
            case HTTP_B_TRAILER_LF:
                if (c != '\n') return http_body_fail(body, PROVEN_ERR_INVALID_FORMAT);
                body->state = HTTP_B_TRAILER_START;
                break;
            case HTTP_B_FINAL_LF:
                if (c != '\n') return http_body_fail(body, PROVEN_ERR_INVALID_FORMAT);
                body->done = true;
                *done = true;
                *consumed = i + 1;
                return PROVEN_OK;
            default:
                return http_body_fail(body, PROVEN_ERR_INVALID_STATE);
        }
        /* Every byte of framing counts toward a bound: a chunk-size line while one is being
         * read, the trailer section once the last chunk has been seen. */
        body->line_bytes++;
        bool in_trailers = body->state >= HTTP_B_TRAILER_START && body->state <= HTTP_B_FINAL_LF;
        if (body->line_bytes > (in_trailers ? PROVEN_HTTP_MAX_TRAILER_BYTES : PROVEN_HTTP_MAX_CHUNK_LINE)) {
            return http_body_fail(body, PROVEN_ERR_OUT_OF_BOUNDS);
        }
        i++;
    }
    *consumed = i;
    return PROVEN_OK;
}

proven_err_t proven_http_body_keep_trailers(proven_http_body_t *body, proven_mem_mut_t buf) {
    if (!body || !buf.ptr || buf.size == 0) return PROVEN_ERR_INVALID_ARG;
    body->trailers = buf.ptr;
    body->trailer_cap = buf.size < PROVEN_HTTP_MAX_TRAILER_BYTES ? (proven_u32)buf.size : PROVEN_HTTP_MAX_TRAILER_BYTES;
    return PROVEN_OK;
}

proven_err_t proven_http_body_trailers(const proven_http_body_t *body, proven_http_header_t *fields, proven_size_t cap, proven_size_t *count) {
    if (count) *count = 0;
    if (!body || !count || (cap > 0 && !fields)) return PROVEN_ERR_INVALID_ARG;
    if (!body->done) return PROVEN_ERR_INVALID_STATE;
    if (body->kind != PROVEN_HTTP_BODY_CHUNKED) return PROVEN_OK;
    if (!body->trailers) return PROVEN_ERR_INVALID_STATE;
    /* The section through its final LF, which ended the body without being counted. */
    proven_http_cur_t cur = { .p = body->trailers, .n = (proven_size_t)body->line_bytes + 1, .capped = true, .pos = 0 };
    proven_err_t e = http_fields_of(&cur, fields, cap, count, true);
    return e == PROVEN_OK ? PROVEN_OK : e == PROVEN_ERR_OUT_OF_BOUNDS ? e : PROVEN_ERR_INVALID_FORMAT;
}

bool proven_http_trailer_allowed(proven_u8str_view_t name) {
    if (name.size == 0 || name.ptr[0] == ':') return false;
    static const char *const NOT[] = { "Content-Length", "Transfer-Encoding", "Trailer", "Host", "Connection", "Keep-Alive", "TE", "Upgrade",
                                       "Expect", "Max-Forwards", "Cache-Control", "Pragma", "Range", "Authorization", "Proxy-Authorization",
                                       "WWW-Authenticate", "Proxy-Authenticate", "Cookie", "Set-Cookie", "Age", "Date", "Expires", "Location",
                                       "Retry-After", "Vary", "Content-Encoding", "Content-Type", "Content-Range" };
    for (proven_size_t i = 0; i < sizeof NOT / sizeof NOT[0]; ++i) if (proven_u8str_view_eq_ascii_ci(name, http_cstr(NOT[i]))) return false;
    return true;
}

proven_err_t proven_http_body_end(const proven_http_body_t *body) {
    if (!body) return PROVEN_ERR_INVALID_ARG;
    if (body->state == HTTP_B_FAILED) return PROVEN_ERR_INVALID_STATE;
    if (body->done || body->kind == PROVEN_HTTP_BODY_UNTIL_CLOSE) return PROVEN_OK;
    return PROVEN_ERR_NEED_MORE;
}

proven_u64 proven_http_body_received(const proven_http_body_t *body) {
    return body ? body->total : 0;
}

// -----------------------------------------------------------------------------
// Writing
// -----------------------------------------------------------------------------

typedef struct {
    proven_mem_mut_t out;
    proven_size_t len;
    bool full;
} proven_http_w_t;

static void http_put(proven_http_w_t *w, proven_u8str_view_t s) {
    if (w->full || s.size > w->out.size - w->len) { w->full = true; return; }
    for (proven_size_t i = 0; i < s.size; ++i) w->out.ptr[w->len + i] = s.ptr[i];
    w->len += s.size;
}

static void http_put_cstr(proven_http_w_t *w, const char *s) { http_put(w, http_cstr(s)); }

static void http_put_u64(proven_http_w_t *w, proven_u64 v, unsigned base) {
    static const char digits[] = "0123456789abcdef";
    proven_byte_t tmp[20];
    proven_size_t n = 0;
    do { tmp[n++] = (proven_byte_t)digits[v % base]; v /= base; } while (v);
    proven_byte_t rev[20];
    for (proven_size_t i = 0; i < n; ++i) rev[i] = tmp[n - 1 - i];
    http_put(w, (proven_u8str_view_t){ .ptr = rev, .size = n });
}

static bool http_w_begin(proven_http_w_t *w, proven_mem_mut_t out, const proven_size_t *len) {
    if (!len || (out.size > 0 && !out.ptr) || *len > out.size) return false;
    w->out = out;
    w->len = *len;
    w->full = false;
    return true;
}

static proven_err_t http_w_end(const proven_http_w_t *w, proven_size_t *len) {
    if (w->full) return PROVEN_ERR_OUT_OF_BOUNDS;     /* *len untouched: nothing was appended */
    *len = w->len;
    return PROVEN_OK;
}

proven_err_t proven_http_write_request_line(proven_mem_mut_t out, proven_size_t *len,
                                            proven_u8str_view_t method, proven_u8str_view_t target) {
    proven_http_w_t w;
    if (!http_w_begin(&w, out, len)) return PROVEN_ERR_INVALID_ARG;
    if (!http_is_token(method) || target.size == 0) return PROVEN_ERR_INVALID_ARG;
    for (proven_size_t i = 0; i < target.size; ++i) {
        if (target.ptr[i] <= 0x20 || target.ptr[i] >= 0x7f) return PROVEN_ERR_INVALID_ARG;
    }
    http_put(&w, method);
    http_put_cstr(&w, " ");
    http_put(&w, target);
    http_put_cstr(&w, " HTTP/1.1\r\n");
    return http_w_end(&w, len);
}

proven_err_t proven_http_write_status_line(proven_mem_mut_t out, proven_size_t *len,
                                           proven_u16 status, proven_u8str_view_t reason) {
    proven_http_w_t w;
    if (!http_w_begin(&w, out, len)) return PROVEN_ERR_INVALID_ARG;
    if (status < 100 || status > 999) return PROVEN_ERR_INVALID_ARG;
    for (proven_size_t i = 0; i < reason.size; ++i) {
        if (!http_is_field_byte(reason.ptr[i])) return PROVEN_ERR_INVALID_ARG;
    }
    http_put_cstr(&w, "HTTP/1.1 ");
    http_put_u64(&w, status, 10);
    http_put_cstr(&w, " ");
    http_put(&w, reason.size > 0 ? reason : proven_http_reason_phrase(status));
    http_put_cstr(&w, "\r\n");
    return http_w_end(&w, len);
}

proven_err_t proven_http_write_header(proven_mem_mut_t out, proven_size_t *len,
                                      proven_u8str_view_t name, proven_u8str_view_t value) {
    proven_http_w_t w;
    if (!http_w_begin(&w, out, len)) return PROVEN_ERR_INVALID_ARG;
    if (!http_is_token(name) || (value.size > 0 && !value.ptr)) return PROVEN_ERR_INVALID_ARG;
    /* The value is what a caller most often takes from somewhere else - a request, a file, a
     * user. A CR or LF in it would end this field and begin one of the sender's choosing. */
    for (proven_size_t i = 0; i < value.size; ++i) {
        if (!http_is_field_byte(value.ptr[i])) return PROVEN_ERR_INVALID_ARG;
    }
    if (value.size > 0 && (http_is_ows(value.ptr[0]) || http_is_ows(value.ptr[value.size - 1]))) return PROVEN_ERR_INVALID_ARG;
    http_put(&w, name);
    http_put_cstr(&w, ": ");
    http_put(&w, value);
    http_put_cstr(&w, "\r\n");
    return http_w_end(&w, len);
}

proven_err_t proven_http_write_header_u64(proven_mem_mut_t out, proven_size_t *len,
                                          proven_u8str_view_t name, proven_u64 value) {
    proven_http_w_t w;
    if (!http_w_begin(&w, out, len)) return PROVEN_ERR_INVALID_ARG;
    if (!http_is_token(name)) return PROVEN_ERR_INVALID_ARG;
    http_put(&w, name);
    http_put_cstr(&w, ": ");
    http_put_u64(&w, value, 10);
    http_put_cstr(&w, "\r\n");
    return http_w_end(&w, len);
}

proven_err_t proven_http_write_head_end(proven_mem_mut_t out, proven_size_t *len) {
    proven_http_w_t w;
    if (!http_w_begin(&w, out, len)) return PROVEN_ERR_INVALID_ARG;
    http_put_cstr(&w, "\r\n");
    return http_w_end(&w, len);
}

proven_err_t proven_http_write_chunk_begin(proven_mem_mut_t out, proven_size_t *len, proven_u64 size) {
    proven_http_w_t w;
    if (!http_w_begin(&w, out, len)) return PROVEN_ERR_INVALID_ARG;
    if (size == 0) return PROVEN_ERR_INVALID_ARG;
    http_put_u64(&w, size, 16);
    http_put_cstr(&w, "\r\n");
    return http_w_end(&w, len);
}

proven_err_t proven_http_write_chunk_end(proven_mem_mut_t out, proven_size_t *len) {
    return proven_http_write_head_end(out, len);
}

/* For the drivers (declared in proven_internal_http_coding.h): fields that may be sent as
 * trailers - valid names and values that proven_http_trailer_allowed lets through. */
bool proven_http_trailers_valid_(const proven_http_header_t *fields, proven_size_t count);
bool proven_http_trailers_valid_(const proven_http_header_t *fields, proven_size_t count) {
    if (count > 0 && !fields) return false;
    for (proven_size_t i = 0; i < count; ++i) {
        if (!http_is_token(fields[i].name) || !proven_http_trailer_allowed(fields[i].name)) return false;
        if (fields[i].value.size > 0 && !fields[i].value.ptr) return false;
        for (proven_size_t k = 0; k < fields[i].value.size; ++k) if (!http_is_field_byte(fields[i].value.ptr[k])) return false;
    }
    return true;
}

proven_err_t proven_http_write_trailers(proven_mem_mut_t out, proven_size_t *len, const proven_http_header_t *fields, proven_size_t count) {
    if (!len || !proven_http_trailers_valid_(fields, count)) return PROVEN_ERR_INVALID_ARG;
    const proven_size_t start = *len;
    proven_http_w_t w;
    if (!http_w_begin(&w, out, len)) return PROVEN_ERR_INVALID_ARG;
    http_put_cstr(&w, "0\r\n");
    proven_err_t e = http_w_end(&w, len);
    for (proven_size_t i = 0; i < count && e == PROVEN_OK; ++i) e = proven_http_write_header(out, len, fields[i].name, fields[i].value);
    if (e == PROVEN_OK) e = proven_http_write_head_end(out, len);
    if (e != PROVEN_OK) *len = start;
    return e;
}

proven_err_t proven_http_write_last_chunk(proven_mem_mut_t out, proven_size_t *len) {
    proven_http_w_t w;
    if (!http_w_begin(&w, out, len)) return PROVEN_ERR_INVALID_ARG;
    http_put_cstr(&w, "0\r\n\r\n");
    return http_w_end(&w, len);
}

// -----------------------------------------------------------------------------
// Ranges
// -----------------------------------------------------------------------------

proven_err_t proven_http_write_range(proven_mem_mut_t out, proven_size_t *len, proven_u64 first, proven_u64 last) {
    proven_http_w_t w;
    if (!http_w_begin(&w, out, len)) return PROVEN_ERR_INVALID_ARG;
    if (last < first) return PROVEN_ERR_INVALID_ARG;
    http_put_cstr(&w, "Range: bytes=");
    http_put_u64(&w, first, 10);
    http_put_cstr(&w, "-");
    if (last != PROVEN_HTTP_RANGE_TO_END) http_put_u64(&w, last, 10);
    http_put_cstr(&w, "\r\n");
    return http_w_end(&w, len);
}

proven_err_t proven_http_write_content_range(proven_mem_mut_t out, proven_size_t *len,
                                             proven_u64 first, proven_u64 last, proven_u64 total) {
    proven_http_w_t w;
    if (!http_w_begin(&w, out, len)) return PROVEN_ERR_INVALID_ARG;
    if (first > last || last >= total) return PROVEN_ERR_INVALID_ARG;
    http_put_cstr(&w, "Content-Range: bytes ");
    http_put_u64(&w, first, 10);
    http_put_cstr(&w, "-");
    http_put_u64(&w, last, 10);
    http_put_cstr(&w, "/");
    http_put_u64(&w, total, 10);
    http_put_cstr(&w, "\r\n");
    return http_w_end(&w, len);
}

/* Decimal digits at s[*pos...], with overflow as "not a number". False when there are none. */
static bool http_take_u64(proven_u8str_view_t s, proven_size_t *pos, proven_u64 *out) {
    proven_size_t i = *pos;
    proven_u64 n = 0;
    while (i < s.size && s.ptr[i] >= '0' && s.ptr[i] <= '9') {
        proven_u64 d = (proven_u64)(s.ptr[i] - '0');
        if (n > (UINT64_MAX - d) / 10u) return false;
        n = n * 10u + d;
        i++;
    }
    if (i == *pos) return false;
    *pos = i;
    *out = n;
    return true;
}

proven_err_t proven_http_range_parse(proven_u8str_view_t value, proven_u64 size, proven_u64 *first, proven_u64 *last) {
    if (!first || !last || (value.size > 0 && !value.ptr)) return PROVEN_ERR_INVALID_ARG;
    proven_size_t eq = 0;
    while (eq < value.size && value.ptr[eq] != '=') eq++;
    if (eq == value.size || eq == 0) return PROVEN_ERR_INVALID_FORMAT;
    proven_u8str_view_t unit = { .ptr = value.ptr, .size = eq };
    if (!http_is_token(unit)) return PROVEN_ERR_INVALID_FORMAT;
    if (!proven_u8str_view_eq_ascii_ci(unit, http_cstr("bytes"))) return PROVEN_ERR_UNSUPPORTED;

    proven_u8str_view_t spec = { .ptr = value.ptr + eq + 1, .size = value.size - eq - 1 };
    for (proven_size_t i = 0; i < spec.size; ++i) if (spec.ptr[i] == ',') return PROVEN_ERR_UNSUPPORTED;
    while (spec.size > 0 && http_is_ows(spec.ptr[0])) { spec.ptr++; spec.size--; }
    while (spec.size > 0 && http_is_ows(spec.ptr[spec.size - 1])) spec.size--;

    proven_size_t pos = 0;
    proven_u64 a = 0, b = 0;
    if (spec.size > 0 && spec.ptr[0] == '-') {
        /* "-n": the last n bytes. */
        pos = 1;
        if (!http_take_u64(spec, &pos, &b) || pos != spec.size) return PROVEN_ERR_INVALID_FORMAT;
        if (b == 0 || size == 0) return PROVEN_ERR_OUT_OF_BOUNDS;
        *first = b >= size ? 0 : size - b;
        *last = size - 1;
        return PROVEN_OK;
    }
    if (!http_take_u64(spec, &pos, &a)) return PROVEN_ERR_INVALID_FORMAT;
    if (pos >= spec.size || spec.ptr[pos] != '-') return PROVEN_ERR_INVALID_FORMAT;
    pos++;
    bool open = pos == spec.size;
    if (!open && (!http_take_u64(spec, &pos, &b) || pos != spec.size)) return PROVEN_ERR_INVALID_FORMAT;
    if (!open && b < a) return PROVEN_ERR_INVALID_FORMAT;
    if (a >= size) return PROVEN_ERR_OUT_OF_BOUNDS;
    *first = a;
    *last = (open || b >= size) ? size - 1 : b;
    return PROVEN_OK;
}

proven_err_t proven_http_content_range_parse(proven_u8str_view_t value, proven_u64 *first, proven_u64 *last,
                                             proven_u64 *total, bool *has_total) {
    if (!first || !last || !total || !has_total || (value.size > 0 && !value.ptr)) return PROVEN_ERR_INVALID_ARG;
    static const char unit[] = "bytes ";
    if (value.size < 6) return PROVEN_ERR_INVALID_FORMAT;
    for (proven_size_t i = 0; i < 6; ++i) if (http_lower(value.ptr[i]) != (proven_byte_t)unit[i]) return PROVEN_ERR_INVALID_FORMAT;
    proven_size_t pos = 6;
    proven_u64 a = 0, b = 0, t = 0;
    if (!http_take_u64(value, &pos, &a) || pos >= value.size || value.ptr[pos] != '-') return PROVEN_ERR_INVALID_FORMAT;
    pos++;
    if (!http_take_u64(value, &pos, &b) || pos >= value.size || value.ptr[pos] != '/') return PROVEN_ERR_INVALID_FORMAT;
    pos++;
    bool known = true;
    if (pos + 1 == value.size && value.ptr[pos] == '*') { known = false; pos++; }
    else if (!http_take_u64(value, &pos, &t)) return PROVEN_ERR_INVALID_FORMAT;
    if (pos != value.size || a > b || (known && b >= t)) return PROVEN_ERR_INVALID_FORMAT;
    *first = a;
    *last = b;
    *total = t;
    *has_total = known;
    return PROVEN_OK;
}

// -----------------------------------------------------------------------------
// Multipart form data
// -----------------------------------------------------------------------------

void proven_http_multipart_boundary(const proven_byte_t random[16], proven_byte_t out[PROVEN_HTTP_BOUNDARY_SIZE]) {
    static const char digits[] = "0123456789abcdef";
    static const char prefix[] = "--proven";                 /* 8 characters, then 32 of hex */
    if (!random || !out) return;
    for (proven_size_t i = 0; i < 8; ++i) out[i] = (proven_byte_t)prefix[i];
    for (proven_size_t i = 0; i < 16; ++i) {
        out[8 + i * 2] = (proven_byte_t)digits[random[i] >> 4];
        out[9 + i * 2] = (proven_byte_t)digits[random[i] & 0xf];
    }
}

/* RFC 2046 boundary characters, less the ones that would need the parameter quoted. */
static bool http_boundary_ok(proven_u8str_view_t b) {
    if (b.size == 0 || b.size > 70) return false;
    for (proven_size_t i = 0; i < b.size; ++i) {
        proven_byte_t c = b.ptr[i];
        bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  c == '-' || c == '_' || c == '.' || c == '+';
        if (!ok) return false;
    }
    return true;
}

proven_err_t proven_http_multipart_write_content_type(proven_mem_mut_t out, proven_size_t *len, proven_u8str_view_t boundary) {
    proven_http_w_t w;
    if (!http_w_begin(&w, out, len)) return PROVEN_ERR_INVALID_ARG;
    if (!http_boundary_ok(boundary)) return PROVEN_ERR_INVALID_ARG;
    http_put_cstr(&w, "Content-Type: multipart/form-data; boundary=");
    http_put(&w, boundary);
    http_put_cstr(&w, "\r\n");
    return http_w_end(&w, len);
}

/* A name inside a quoted string: '"', CR and LF percent-encoded, as browsers write them. */
static void http_put_quoted_name(proven_http_w_t *w, proven_u8str_view_t s) {
    for (proven_size_t i = 0; i < s.size; ++i) {
        proven_byte_t c = s.ptr[i];
        if (c == '"') http_put_cstr(w, "%22");
        else if (c == '\r') http_put_cstr(w, "%0D");
        else if (c == '\n') http_put_cstr(w, "%0A");
        else http_put(w, (proven_u8str_view_t){ .ptr = &s.ptr[i], .size = 1 });
    }
}

proven_err_t proven_http_multipart_write_part(proven_mem_mut_t out, proven_size_t *len, proven_u8str_view_t boundary,
                                              proven_u8str_view_t name, proven_u8str_view_t filename,
                                              proven_u8str_view_t content_type) {
    proven_http_w_t w;
    if (!http_w_begin(&w, out, len)) return PROVEN_ERR_INVALID_ARG;
    if (!http_boundary_ok(boundary) || name.size == 0) return PROVEN_ERR_INVALID_ARG;
    for (proven_size_t i = 0; i < name.size; ++i) if (name.ptr[i] == 0) return PROVEN_ERR_INVALID_ARG;
    for (proven_size_t i = 0; i < filename.size; ++i) if (filename.ptr[i] == 0) return PROVEN_ERR_INVALID_ARG;
    for (proven_size_t i = 0; i < content_type.size; ++i) {
        if (!http_is_field_byte(content_type.ptr[i])) return PROVEN_ERR_INVALID_ARG;
    }
    http_put_cstr(&w, "--");
    http_put(&w, boundary);
    http_put_cstr(&w, "\r\nContent-Disposition: form-data; name=\"");
    http_put_quoted_name(&w, name);
    http_put_cstr(&w, "\"");
    if (filename.size > 0) {
        http_put_cstr(&w, "; filename=\"");
        http_put_quoted_name(&w, filename);
        http_put_cstr(&w, "\"");
    }
    http_put_cstr(&w, "\r\n");
    if (content_type.size > 0) {
        http_put_cstr(&w, "Content-Type: ");
        http_put(&w, content_type);
        http_put_cstr(&w, "\r\n");
    }
    http_put_cstr(&w, "\r\n");
    return http_w_end(&w, len);
}

proven_err_t proven_http_multipart_write_part_end(proven_mem_mut_t out, proven_size_t *len) {
    return proven_http_write_head_end(out, len);
}

proven_err_t proven_http_multipart_write_end(proven_mem_mut_t out, proven_size_t *len, proven_u8str_view_t boundary) {
    proven_http_w_t w;
    if (!http_w_begin(&w, out, len)) return PROVEN_ERR_INVALID_ARG;
    if (!http_boundary_ok(boundary)) return PROVEN_ERR_INVALID_ARG;
    http_put_cstr(&w, "--");
    http_put(&w, boundary);
    http_put_cstr(&w, "--\r\n");
    return http_w_end(&w, len);
}

// -----------------------------------------------------------------------------
// Reading multipart bodies
// -----------------------------------------------------------------------------

/* RFC 2046's bchars: what a boundary may hold (a space too, but not last). */
static bool http_bchar(proven_byte_t c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           c == '\'' || c == '(' || c == ')' || c == '+' || c == '_' || c == ',' || c == '-' || c == '.' ||
           c == '/' || c == ':' || c == '=' || c == '?' || c == ' ';
}

static bool http_boundary_valid(proven_u8str_view_t b) {
    if (b.size == 0 || b.size > PROVEN_HTTP_BOUNDARY_MAX || b.ptr[b.size - 1] == ' ') return false;
    for (proven_size_t i = 0; i < b.size; ++i) if (!http_bchar(b.ptr[i])) return false;
    return true;
}

/* One `; name=value` parameter from `*at` in `v`: the name, and the value - a token, or a quoted
 * string without its quotes. Returns false at the end; `*bad` for a malformed one, `*escaped` for a
 * quoted value with a backslash. */
static bool http_param_next(proven_u8str_view_t v, proven_size_t *at, proven_u8str_view_t *name, proven_u8str_view_t *value,
                            bool *bad, bool *escaped) {
    proven_size_t i = *at;
    while (i < v.size && http_is_ows(v.ptr[i])) i++;
    if (i == v.size) { *at = i; return false; }
    if (v.ptr[i] != ';') { *bad = true; return false; }
    i++;
    while (i < v.size && http_is_ows(v.ptr[i])) i++;
    const proven_size_t n0 = i;
    while (i < v.size && http_is_tchar(v.ptr[i])) i++;
    if (i == n0 || i == v.size || v.ptr[i] != '=') { *bad = true; return false; }
    *name = (proven_u8str_view_t){ .ptr = v.ptr + n0, .size = i - n0 };
    i++;
    if (i < v.size && v.ptr[i] == '"') {
        const proven_size_t v0 = ++i;
        while (i < v.size && v.ptr[i] != '"') {
            if (v.ptr[i] == '\\') *escaped = true;
            if (v.ptr[i] < 0x20 && v.ptr[i] != '\t') { *bad = true; return false; }
            i++;
        }
        if (i == v.size) { *bad = true; return false; }
        *value = (proven_u8str_view_t){ .ptr = v.ptr + v0, .size = i - v0 };
        i++;
    } else {
        const proven_size_t v0 = i;
        while (i < v.size && http_is_tchar(v.ptr[i])) i++;
        if (i == v0) { *bad = true; return false; }
        *value = (proven_u8str_view_t){ .ptr = v.ptr + v0, .size = i - v0 };
    }
    *at = i;
    return true;
}

static bool http_eq_ci_lit(proven_u8str_view_t a, const char *lit) {
    return proven_u8str_view_eq_ascii_ci(a, http_cstr(lit));
}

proven_err_t proven_http_multipart_boundary_parse(proven_u8str_view_t content_type, proven_u8str_view_t *boundary) {
    if (boundary) *boundary = (proven_u8str_view_t){0};
    if (!boundary || (content_type.size > 0 && !content_type.ptr)) return PROVEN_ERR_INVALID_ARG;
    const proven_u8str_view_t v = content_type;
    proven_size_t i = 0;
    while (i < v.size && http_is_ows(v.ptr[i])) i++;
    static const char multi[] = "multipart/";
    if (v.size - i < sizeof multi - 1 || !http_eq_ci_lit((proven_u8str_view_t){ .ptr = v.ptr + i, .size = sizeof multi - 1 }, multi)) return PROVEN_ERR_NOT_FOUND;
    i += sizeof multi - 1;
    const proven_size_t sub = i;
    while (i < v.size && http_is_tchar(v.ptr[i])) i++;
    if (i == sub) return PROVEN_ERR_NOT_FOUND;
    proven_u8str_view_t name, value;
    bool bad = false, escaped = false, found = false;
    while (http_param_next(v, &i, &name, &value, &bad, &escaped)) {
        if (http_eq_ci_lit(name, "boundary")) {
            if (found) return PROVEN_ERR_INVALID_FORMAT;
            found = true;
            *boundary = value;
            if (escaped) return PROVEN_ERR_INVALID_FORMAT;
        }
        escaped = false;
    }
    if (bad) { *boundary = (proven_u8str_view_t){0}; return PROVEN_ERR_INVALID_FORMAT; }
    if (!found) return PROVEN_ERR_NOT_FOUND;
    if (!http_boundary_valid(*boundary)) { *boundary = (proven_u8str_view_t){0}; return PROVEN_ERR_INVALID_FORMAT; }
    return PROVEN_OK;
}

enum { MP_PREAMBLE, MP_AFTER, MP_AFTER_DASH, MP_AFTER_CR, MP_HEAD, MP_BODY, MP_EPILOGUE, MP_FAILED };

proven_err_t proven_http_multipart_init(proven_http_multipart_t *r, proven_u8str_view_t boundary, proven_mem_mut_t head,
                                        proven_http_header_t *fields, proven_size_t field_cap) {
    if (!r) return PROVEN_ERR_INVALID_ARG;
    *r = (proven_http_multipart_t){ .state = MP_FAILED };
    if ((boundary.size > 0 && !boundary.ptr) || !http_boundary_valid(boundary) || !head.ptr || head.size < 2 || !fields || field_cap == 0) return PROVEN_ERR_INVALID_ARG;
    r->delim[0] = '\r'; r->delim[1] = '\n'; r->delim[2] = '-'; r->delim[3] = '-';
    for (proven_size_t i = 0; i < boundary.size; ++i) r->delim[4 + i] = boundary.ptr[i];
    r->delim_len = (proven_u8)(4 + boundary.size);
    /* The first boundary line may begin the body: read as if a line ended just before it. */
    r->hold[0] = '\r'; r->hold[1] = '\n';
    r->held = 2;
    r->head = head;
    r->fields = fields;
    r->field_cap = field_cap;
    r->state = MP_PREAMBLE;
    return PROVEN_OK;
}

static proven_err_t mp_fail(proven_http_multipart_t *r, proven_err_t e) {
    r->state = MP_FAILED;
    return e;
}

proven_err_t proven_http_multipart_feed(proven_http_multipart_t *r, proven_mem_view_t in, proven_size_t *used, proven_http_multipart_event_t *ev) {
    if (used) *used = 0;
    if (ev) *ev = (proven_http_multipart_event_t){0};
    if (!r || !used || !ev || (in.size > 0 && !in.ptr)) return PROVEN_ERR_INVALID_ARG;
    if (r->state == MP_FAILED) return PROVEN_ERR_INVALID_FORMAT;
    const proven_byte_t *const d = r->delim;
    const proven_size_t dn = r->delim_len;
    proven_size_t at = 0;
    for (;;) {
        switch (r->state) {
            case MP_PREAMBLE:
            case MP_BODY: {
                const bool body = r->state == MP_BODY;
                if (r->held > 0) {
                    /* Held bytes are the start of the boundary line: does the input go on with it? */
                    const proven_size_t need = dn - r->held, avail = in.size - at, k = need < avail ? need : avail;
                    proven_size_t m = 0;
                    while (m < k && in.ptr[at + m] == d[r->held + m]) m++;
                    if (m == k && k == need) {
                        at += k;
                        r->held = 0;
                        r->state = MP_AFTER;
                        if (body) { *used = at; ev->kind = PROVEN_HTTP_MULTIPART_PART_END; return PROVEN_OK; }
                        continue;
                    }
                    if (m == k) {
                        for (proven_size_t i = 0; i < k; ++i) r->hold[r->held + i] = in.ptr[at + i];
                        r->held = (proven_u8)(r->held + k);
                        at += k;
                        *used = at;
                        return PROVEN_OK;
                    }
                    /* Not a boundary line: the held bytes are data, all of them - they are the start of
                     * CR LF - - boundary, and a boundary has no CR, so no later one of them can begin
                     * another boundary line. */
                    const proven_size_t out = r->held;
                    for (proven_size_t i = 0; i < out; ++i) r->spill[i] = r->hold[i];
                    r->held = 0;
                    if (body) {
                        *used = at;
                        ev->kind = PROVEN_HTTP_MULTIPART_DATA;
                        ev->data = (proven_mem_view_t){ .ptr = r->spill, .size = out };
                        return PROVEN_OK;
                    }
                    continue;
                }
                if (at == in.size) { *used = at; return PROVEN_OK; }
                /* The first place a boundary line could begin, whole or cut off by the end of the input. */
                const proven_byte_t *p = in.ptr + at, *const end = in.ptr + in.size;
                proven_size_t found = (proven_size_t)(end - p);
                bool whole = false;
                for (const proven_byte_t *q = p; q < end; ++q) {
                    if (*q != '\r') continue;
                    proven_size_t m = 0;
                    while (m < dn && q + m < end && q[m] == d[m]) m++;
                    if (m == dn) { found = (proven_size_t)(q - p); whole = true; break; }
                    if (q + m == end) { found = (proven_size_t)(q - p); break; }
                }
                if (found > 0) {
                    at += found;
                    if (body) {
                        *used = at;
                        ev->kind = PROVEN_HTTP_MULTIPART_DATA;
                        ev->data = (proven_mem_view_t){ .ptr = p, .size = found };
                        return PROVEN_OK;
                    }
                    continue;
                }
                if (whole) {
                    at += dn;
                    r->state = MP_AFTER;
                    if (body) { *used = at; ev->kind = PROVEN_HTTP_MULTIPART_PART_END; return PROVEN_OK; }
                    continue;
                }
                /* The input ends inside what may be a boundary line: hold it. */
                const proven_size_t k = (proven_size_t)(end - p);
                for (proven_size_t i = 0; i < k; ++i) r->hold[i] = p[i];
                r->held = (proven_u8)k;
                at = in.size;
                *used = at;
                return PROVEN_OK;
            }
            case MP_AFTER:
            case MP_AFTER_DASH:
            case MP_AFTER_CR: {
                if (at == in.size) { *used = at; return PROVEN_OK; }
                const proven_byte_t c = in.ptr[at++];
                if (r->state == MP_AFTER_DASH) {
                    if (c != '-') { *used = at; return mp_fail(r, PROVEN_ERR_INVALID_FORMAT); }
                    r->state = MP_EPILOGUE;
                    *used = at;
                    ev->kind = PROVEN_HTTP_MULTIPART_END;
                    return PROVEN_OK;
                }
                if (r->state == MP_AFTER_CR) {
                    if (c != '\n') { *used = at; return mp_fail(r, PROVEN_ERR_INVALID_FORMAT); }
                    r->state = MP_HEAD;
                    r->head_len = 0;
                    continue;
                }
                if (c == '-') r->state = MP_AFTER_DASH;
                else if (c == '\r') r->state = MP_AFTER_CR;
                else if (c != ' ' && c != '\t') { *used = at; return mp_fail(r, PROVEN_ERR_INVALID_FORMAT); }
                continue;
            }
            case MP_HEAD: {
                /* Into the head buffer until the empty line. */
                while (at < in.size) {
                    if (r->head_len == r->head.size) { *used = at; return mp_fail(r, PROVEN_ERR_OUT_OF_BOUNDS); }
                    r->head.ptr[r->head_len++] = in.ptr[at++];
                    const proven_byte_t *h = r->head.ptr;
                    const proven_size_t n = r->head_len;
                    /* A bare LF can never end the head: refused now, not after the rest of the body. */
                    if (h[n - 1] == '\n' && (n < 2 || h[n - 2] != '\r')) { *used = at; return mp_fail(r, PROVEN_ERR_INVALID_FORMAT); }
                    if ((n == 2 && h[0] == '\r' && h[1] == '\n') || (n >= 4 && h[n - 4] == '\r' && h[n - 3] == '\n' && h[n - 2] == '\r' && h[n - 1] == '\n')) {
                        proven_http_cur_t c = { .p = h, .n = n, .capped = true, .pos = 0 };
                        proven_size_t count = 0;
                        const proven_err_t e = http_fields(&c, r->fields, r->field_cap, &count);
                        if (e != PROVEN_OK || c.pos != n) { *used = at; return mp_fail(r, e == PROVEN_ERR_OUT_OF_BOUNDS ? e : PROVEN_ERR_INVALID_FORMAT); }
                        r->state = MP_BODY;
                        *used = at;
                        ev->kind = PROVEN_HTTP_MULTIPART_PART;
                        ev->fields = r->fields;
                        ev->field_count = count;
                        return PROVEN_OK;
                    }
                }
                *used = at;
                return PROVEN_OK;
            }
            case MP_EPILOGUE:
                *used = in.size;
                return PROVEN_OK;
            default:
                return PROVEN_ERR_INVALID_FORMAT;
        }
    }
}

proven_err_t proven_http_multipart_end(const proven_http_multipart_t *r) {
    if (!r) return PROVEN_ERR_INVALID_ARG;
    return r->state == MP_EPILOGUE ? PROVEN_OK : PROVEN_ERR_INVALID_FORMAT;
}

proven_err_t proven_http_content_disposition_parse(proven_u8str_view_t value, proven_http_disposition_t *out) {
    if (out) *out = (proven_http_disposition_t){0};
    if (!out || (value.size > 0 && !value.ptr)) return PROVEN_ERR_INVALID_ARG;
    proven_size_t i = 0;
    while (i < value.size && http_is_ows(value.ptr[i])) i++;
    const proven_size_t t0 = i;
    while (i < value.size && http_is_tchar(value.ptr[i])) i++;
    if (i == t0) return PROVEN_ERR_INVALID_FORMAT;
    out->type = (proven_u8str_view_t){ .ptr = value.ptr + t0, .size = i - t0 };
    proven_u8str_view_t name, v;
    bool bad = false, escaped = false;
    while (http_param_next(value, &i, &name, &v, &bad, &escaped)) {
        bool *has = (void *)0;
        proven_u8str_view_t *slot = (void *)0;
        if (http_eq_ci_lit(name, "name")) { has = &out->has_name; slot = &out->name; }
        else if (http_eq_ci_lit(name, "filename")) { has = &out->has_filename; slot = &out->filename; }
        else if (http_eq_ci_lit(name, "filename*")) { has = &out->has_filename_ext; slot = &out->filename_ext; }
        if (slot) {
            if (*has) { *out = (proven_http_disposition_t){0}; return PROVEN_ERR_INVALID_FORMAT; }
            if (escaped) { *out = (proven_http_disposition_t){0}; return PROVEN_ERR_UNSUPPORTED; }
            *has = true;
            *slot = v;
        }
        escaped = false;
    }
    if (bad) { *out = (proven_http_disposition_t){0}; return PROVEN_ERR_INVALID_FORMAT; }
    return PROVEN_OK;
}

// -----------------------------------------------------------------------------
// Several ranges
// -----------------------------------------------------------------------------

proven_err_t proven_http_range_parse_list(proven_u8str_view_t value, proven_u64 size, proven_http_range_t *ranges,
                                          proven_size_t cap, proven_size_t *count) {
    if (count) *count = 0;
    if (!count || !ranges || cap == 0 || (value.size > 0 && !value.ptr)) return PROVEN_ERR_INVALID_ARG;
    proven_size_t i = 0;
    while (i < value.size && http_is_ows(value.ptr[i])) i++;
    const proven_size_t u0 = i;
    while (i < value.size && http_is_tchar(value.ptr[i])) i++;
    if (i == u0 || i == value.size || value.ptr[i] != '=') return PROVEN_ERR_INVALID_FORMAT;
    if (!http_eq_ci_lit((proven_u8str_view_t){ .ptr = value.ptr + u0, .size = i - u0 }, "bytes")) return PROVEN_ERR_UNSUPPORTED;
    i++;
    proven_size_t n = 0, specs = 0;
    proven_u64 asked = 0;
    bool more = true;
    while (more) {
        while (i < value.size && http_is_ows(value.ptr[i])) i++;
        /* An empty element between commas is allowed by the list rule. */
        if (i < value.size && value.ptr[i] == ',') { i++; continue; }
        if (i == value.size) break;
        proven_u64 a = 0, b = 0;
        bool has_a = false, has_b = false;
        if (value.ptr[i] != '-') { if (!http_take_u64(value, &i, &a)) return PROVEN_ERR_INVALID_FORMAT; has_a = true; }
        if (i == value.size || value.ptr[i] != '-') return PROVEN_ERR_INVALID_FORMAT;
        i++;
        if (i < value.size && value.ptr[i] >= '0' && value.ptr[i] <= '9') { if (!http_take_u64(value, &i, &b)) return PROVEN_ERR_INVALID_FORMAT; has_b = true; }
        if (!has_a && !has_b) return PROVEN_ERR_INVALID_FORMAT;
        if (has_a && has_b && b < a) return PROVEN_ERR_INVALID_FORMAT;
        while (i < value.size && http_is_ows(value.ptr[i])) i++;
        if (i < value.size) { if (value.ptr[i] != ',') return PROVEN_ERR_INVALID_FORMAT; i++; }
        else more = false;
        if (++specs > cap) return PROVEN_ERR_UNSUPPORTED;
        /* Resolved against the size: a suffix of b bytes, or a..b cut to the end. */
        proven_u64 first, last;
        if (!has_a) {
            if (b == 0 || size == 0) continue;
            first = b >= size ? 0 : size - b;
            last = size - 1;
        } else {
            if (a >= size) continue;
            first = a;
            last = has_b && b < size ? b : size - 1;
        }
        asked += last - first + 1;
        if (asked > size && specs > 1) return PROVEN_ERR_UNSUPPORTED;
        ranges[n++] = (proven_http_range_t){ first, last };
    }
    if (specs == 0) return PROVEN_ERR_INVALID_FORMAT;
    if (n == 0) return PROVEN_ERR_OUT_OF_BOUNDS;
    /* In order of where they start, then those that overlap or touch made one. */
    for (proven_size_t k = 1; k < n; ++k) {
        const proven_http_range_t x = ranges[k];
        proven_size_t j = k;
        while (j > 0 && ranges[j - 1].first > x.first) { ranges[j] = ranges[j - 1]; j--; }
        ranges[j] = x;
    }
    proven_size_t m = 0;
    for (proven_size_t k = 1; k < n; ++k) {
        if (ranges[k].first <= ranges[m].last + 1) { if (ranges[k].last > ranges[m].last) ranges[m].last = ranges[k].last; }
        else ranges[++m] = ranges[k];
    }
    *count = m + 1;
    return PROVEN_OK;
}

proven_err_t proven_http_byteranges_write_content_type(proven_mem_mut_t out, proven_size_t *len, proven_u8str_view_t boundary) {
    proven_http_w_t w;
    if (!http_w_begin(&w, out, len)) return PROVEN_ERR_INVALID_ARG;
    if (!http_boundary_ok(boundary)) return PROVEN_ERR_INVALID_ARG;
    http_put_cstr(&w, "Content-Type: multipart/byteranges; boundary=");
    http_put(&w, boundary);
    http_put_cstr(&w, "\r\n");
    return http_w_end(&w, len);
}

proven_err_t proven_http_byteranges_write_part(proven_mem_mut_t out, proven_size_t *len, proven_u8str_view_t boundary,
                                               proven_u8str_view_t content_type, proven_u64 first, proven_u64 last, proven_u64 size) {
    proven_http_w_t w;
    if (!http_w_begin(&w, out, len)) return PROVEN_ERR_INVALID_ARG;
    if (!http_boundary_ok(boundary) || first > last || last >= size) return PROVEN_ERR_INVALID_ARG;
    for (proven_size_t i = 0; i < content_type.size; ++i) if (!http_is_field_byte(content_type.ptr[i])) return PROVEN_ERR_INVALID_ARG;
    http_put_cstr(&w, "--");
    http_put(&w, boundary);
    http_put_cstr(&w, "\r\n");
    if (content_type.size > 0) {
        http_put_cstr(&w, "Content-Type: ");
        http_put(&w, content_type);
        http_put_cstr(&w, "\r\n");
    }
    http_put_cstr(&w, "Content-Range: bytes ");
    http_put_u64(&w, first, 10);
    http_put_cstr(&w, "-");
    http_put_u64(&w, last, 10);
    http_put_cstr(&w, "/");
    http_put_u64(&w, size, 10);
    http_put_cstr(&w, "\r\n\r\n");
    return http_w_end(&w, len);
}

// -----------------------------------------------------------------------------
// Dates
// -----------------------------------------------------------------------------

static const char http_wkday[7][4] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *const http_weekday[7] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
static const char http_month[12][4] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

/* Days since 1970-01-01 for a proleptic Gregorian date, and back. The arithmetic is the
 * well-known era-based form; years here are 1970..9999, so nothing is negative. */
static proven_i64 http_days_from_civil(proven_i64 y, unsigned m, unsigned d) {
    y -= m <= 2;
    proven_i64 era = y / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153u * (m > 2 ? m - 3 : m + 9) + 2u) / 5u + d - 1u;
    unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + (proven_i64)doe - 719468;
}

static void http_civil_from_days(proven_i64 z, proven_i64 *y, unsigned *m, unsigned *d) {
    z += 719468;
    proven_i64 era = z / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    proven_i64 yy = (proven_i64)yoe + era * 400;
    unsigned doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    unsigned mp = (5u * doy + 2u) / 153u;
    *d = doy - (153u * mp + 2u) / 5u + 1u;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = yy + (*m <= 2);
}

static unsigned http_days_in_month(proven_i64 y, unsigned m) {
    static const unsigned days[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return (m == 2 && leap) ? 29u : days[m - 1];
}

#define HTTP_NS_PER_SEC ((proven_i64)1000000000)
/* The last whole second a proven_time_t can hold: nanoseconds in 64 bits end on 2262-04-11. */
#define HTTP_MAX_EPOCH_SEC (INT64_MAX / HTTP_NS_PER_SEC)

proven_err_t proven_http_date_format(proven_time_t wall_ns, proven_byte_t out[PROVEN_HTTP_DATE_SIZE]) {
    if (!out || wall_ns < 0) return PROVEN_ERR_INVALID_ARG;
    proven_i64 sec = wall_ns / HTTP_NS_PER_SEC;
    proven_i64 days = sec / 86400;
    unsigned tod = (unsigned)(sec % 86400);
    proven_i64 y;
    unsigned m, d;
    http_civil_from_days(days, &y, &m, &d);
    unsigned wd = (unsigned)((days + 4) % 7);               /* 1970-01-01 was a Thursday */

    proven_size_t n = 0;
    for (int i = 0; i < 3; ++i) out[n++] = (proven_byte_t)http_wkday[wd][i];
    out[n++] = ','; out[n++] = ' ';
    out[n++] = (proven_byte_t)('0' + d / 10); out[n++] = (proven_byte_t)('0' + d % 10);
    out[n++] = ' ';
    for (int i = 0; i < 3; ++i) out[n++] = (proven_byte_t)http_month[m - 1][i];
    out[n++] = ' ';
    out[n++] = (proven_byte_t)('0' + (y / 1000) % 10); out[n++] = (proven_byte_t)('0' + (y / 100) % 10);
    out[n++] = (proven_byte_t)('0' + (y / 10) % 10);   out[n++] = (proven_byte_t)('0' + y % 10);
    out[n++] = ' ';
    unsigned hh = tod / 3600, mi = (tod / 60) % 60, ss = tod % 60;
    out[n++] = (proven_byte_t)('0' + hh / 10); out[n++] = (proven_byte_t)('0' + hh % 10); out[n++] = ':';
    out[n++] = (proven_byte_t)('0' + mi / 10); out[n++] = (proven_byte_t)('0' + mi % 10); out[n++] = ':';
    out[n++] = (proven_byte_t)('0' + ss / 10); out[n++] = (proven_byte_t)('0' + ss % 10);
    out[n++] = ' '; out[n++] = 'G'; out[n++] = 'M'; out[n++] = 'T';
    return PROVEN_OK;
}

typedef struct {
    const proven_byte_t *p;
    proven_size_t n;
    proven_size_t pos;
    bool bad;
} proven_http_dp_t;

static void http_dp_lit(proven_http_dp_t *t, const char *lit) {
    for (proven_size_t i = 0; lit[i] != '\0'; ++i) {
        if (t->pos >= t->n || t->p[t->pos] != (proven_byte_t)lit[i]) { t->bad = true; return; }
        t->pos++;
    }
}

static unsigned http_dp_num(proven_http_dp_t *t, unsigned digits) {
    unsigned v = 0;
    for (unsigned i = 0; i < digits; ++i) {
        if (t->pos >= t->n || t->p[t->pos] < '0' || t->p[t->pos] > '9') { t->bad = true; return 0; }
        v = v * 10u + (unsigned)(t->p[t->pos] - '0');
        t->pos++;
    }
    return v;
}

/* Which of `count` names, each `len` bytes (0: NUL-terminated, of any length), starts here. */
static int http_dp_name3(proven_http_dp_t *t, const char names[][4], int count) {
    if (t->n - t->pos < 3) { t->bad = true; return 0; }
    for (int i = 0; i < count; ++i) {
        if (t->p[t->pos] == (proven_byte_t)names[i][0] && t->p[t->pos + 1] == (proven_byte_t)names[i][1] &&
            t->p[t->pos + 2] == (proven_byte_t)names[i][2]) {
            t->pos += 3;
            return i;
        }
    }
    t->bad = true;
    return 0;
}

static void http_dp_time(proven_http_dp_t *t, unsigned *hh, unsigned *mi, unsigned *ss) {
    *hh = http_dp_num(t, 2); http_dp_lit(t, ":");
    *mi = http_dp_num(t, 2); http_dp_lit(t, ":");
    *ss = http_dp_num(t, 2);
}

proven_err_t proven_http_date_parse(proven_u8str_view_t text, proven_time_t now_ns, proven_time_t *out_wall_ns) {
    if (!out_wall_ns || (text.size > 0 && !text.ptr)) return PROVEN_ERR_INVALID_ARG;
    proven_http_dp_t t = { .p = text.ptr, .n = text.size, .pos = 0, .bad = false };
    unsigned wd = 0, d = 0, m = 0, hh = 0, mi = 0, ss = 0;
    proven_i64 y = 0;
    bool two_digit_year = false;

    /* The forms are told apart by what follows the weekday name. */
    proven_size_t comma = 0;
    while (comma < text.size && text.ptr[comma] != ',' && text.ptr[comma] != ' ') comma++;
    if (comma >= text.size) return PROVEN_ERR_INVALID_FORMAT;

    if (text.ptr[comma] == ',' && comma == 3) {
        /* IMF-fixdate: Sun, 06 Nov 1994 08:49:37 GMT */
        wd = (unsigned)http_dp_name3(&t, http_wkday, 7);
        http_dp_lit(&t, ", ");
        d = http_dp_num(&t, 2); http_dp_lit(&t, " ");
        m = (unsigned)http_dp_name3(&t, http_month, 12) + 1u; http_dp_lit(&t, " ");
        y = (proven_i64)http_dp_num(&t, 4); http_dp_lit(&t, " ");
        http_dp_time(&t, &hh, &mi, &ss);
        http_dp_lit(&t, " GMT");
    } else if (text.ptr[comma] == ',') {
        /* RFC 850: Sunday, 06-Nov-94 08:49:37 GMT */
        int found = -1;
        for (int i = 0; i < 7; ++i) {
            proven_u8str_view_t name = http_cstr(http_weekday[i]);
            if (name.size == comma && http_eq(name, (proven_u8str_view_t){ .ptr = text.ptr, .size = comma })) found = i;
        }
        if (found < 0) return PROVEN_ERR_INVALID_FORMAT;
        wd = (unsigned)found;
        t.pos = comma;
        http_dp_lit(&t, ", ");
        d = http_dp_num(&t, 2); http_dp_lit(&t, "-");
        m = (unsigned)http_dp_name3(&t, http_month, 12) + 1u; http_dp_lit(&t, "-");
        y = (proven_i64)http_dp_num(&t, 2); http_dp_lit(&t, " ");
        two_digit_year = true;
        http_dp_time(&t, &hh, &mi, &ss);
        http_dp_lit(&t, " GMT");
    } else {
        /* asctime: Sun Nov  6 08:49:37 1994 - the day is space-padded, not zero-padded. */
        wd = (unsigned)http_dp_name3(&t, http_wkday, 7);
        http_dp_lit(&t, " ");
        m = (unsigned)http_dp_name3(&t, http_month, 12) + 1u; http_dp_lit(&t, " ");
        if (t.pos < t.n && t.p[t.pos] == ' ') { t.pos++; d = http_dp_num(&t, 1); }
        else d = http_dp_num(&t, 2);
        http_dp_lit(&t, " ");
        http_dp_time(&t, &hh, &mi, &ss);
        http_dp_lit(&t, " ");
        y = (proven_i64)http_dp_num(&t, 4);
    }
    if (t.bad || t.pos != t.n) return PROVEN_ERR_INVALID_FORMAT;

    if (two_digit_year) {
        /* RFC 9110 section 5.6.7: a two-digit year that appears to be more than 50 years in
         * the future is the most recent past year with the same last two digits. */
        proven_i64 now_y;
        unsigned nm, nd;
        http_civil_from_days((now_ns < 0 ? 0 : now_ns / HTTP_NS_PER_SEC) / 86400, &now_y, &nm, &nd);
        proven_i64 full = now_y - (now_y % 100) + y;
        if (full > now_y + 50) full -= 100;
        y = full;
    }
    if (y < 1970 || y > 9999 || m < 1 || m > 12 || d < 1 || d > http_days_in_month(y, m)) return PROVEN_ERR_INVALID_FORMAT;
    /* 60 is a leap second, which the grammar allows; it is read as the second after :59. */
    if (hh > 23 || mi > 59 || ss > 60) return PROVEN_ERR_INVALID_FORMAT;

    proven_i64 days = http_days_from_civil(y, m, d);
    if ((unsigned)((days + 4) % 7) != wd) return PROVEN_ERR_INVALID_FORMAT;
    proven_i64 sec = days * 86400 + (proven_i64)hh * 3600 + (proven_i64)mi * 60 + (proven_i64)ss;
    /* A valid date that the time type cannot hold. Servers do send them: "Expires" in the year
     * 9999 means "never", and the caller should be able to tell that from a malformed date. */
    if (sec > HTTP_MAX_EPOCH_SEC) return PROVEN_ERR_OVERFLOW;
    *out_wall_ns = sec * HTTP_NS_PER_SEC;
    return PROVEN_OK;
}
