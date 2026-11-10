#include "proven/http_auth.h"
#include "proven/encode.h"
#include "proven/hash.h"
#include "proven/sha2.h"
#include "proven/hash_legacy.h"

/* RFC 7617 (Basic) and RFC 7616 (Digest), written from those documents. Pure computation. */

static bool auth_is_tchar(proven_byte_t c) {
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return true;
    switch (c) {
        case '!': case '#': case '$': case '%': case '&': case '\'': case '*': case '+':
        case '-': case '.': case '^': case '_': case '`': case '|': case '~':
            return true;
        default:
            return false;
    }
}


static bool auth_eq_nocase(proven_u8str_view_t a, const char *b) { return proven_u8str_view_eq_ascii_ci(a, proven_u8str_view_from_cstr(b)); }


static bool auth_has_control(proven_u8str_view_t s) {
    for (proven_size_t i = 0; i < s.size; ++i) if (s.ptr[i] < 0x20 || s.ptr[i] == 0x7f) return true;
    return false;
}

/* Safe to put between double quotes as it is. */
static bool auth_quotable(proven_u8str_view_t s) {
    for (proven_size_t i = 0; i < s.size; ++i) {
        proven_byte_t c = s.ptr[i];
        if (c < 0x20 || c == 0x7f || c == '"' || c == '\\') return false;
    }
    return true;
}

proven_err_t proven_http_basic_auth(proven_u8str_view_t user, proven_u8str_view_t password,
                                    proven_mem_mut_t out, proven_size_t *written) {
    if (written) *written = 0;
    if (!written || (user.size > 0 && !user.ptr) || (password.size > 0 && !password.ptr) || (out.size > 0 && !out.ptr)) {
        return PROVEN_ERR_INVALID_ARG;
    }
    if (auth_has_control(user) || auth_has_control(password)) return PROVEN_ERR_INVALID_ARG;
    for (proven_size_t i = 0; i < user.size; ++i) if (user.ptr[i] == ':') return PROVEN_ERR_INVALID_ARG;

    /* "user:password" is encoded three bytes at a time from the two views, so no buffer for
     * the joined text is needed - and a password is not copied anywhere but the output. */
    proven_size_t plain = user.size + 1 + password.size;
    if (plain < user.size) return PROVEN_ERR_OVERFLOW;
    proven_size_t encoded = proven_base64_encoded_size(plain);
    if (encoded == PROVEN_SIZE_MAX || encoded > PROVEN_SIZE_MAX - 6) return PROVEN_ERR_OVERFLOW;
    if (out.size < 6 + encoded) return PROVEN_ERR_OUT_OF_BOUNDS;

    static const char prefix[] = "Basic ";
    for (proven_size_t i = 0; i < 6; ++i) out.ptr[i] = (proven_byte_t)prefix[i];
    proven_size_t w = 6;
    for (proven_size_t i = 0; i < plain; i += 3) {
        proven_byte_t group[3];
        proven_size_t n = 0;
        for (proven_size_t k = i; k < plain && n < 3; ++k) {
            group[n++] = k < user.size ? user.ptr[k] : (k == user.size ? (proven_byte_t)':' : password.ptr[k - user.size - 1]);
        }
        proven_size_t got = 0;
        proven_err_t e = proven_base64_encode((proven_mem_view_t){ .ptr = group, .size = n }, out.ptr + w, out.size - w, &got);
        if (e != PROVEN_OK) return e;
        w += got;
    }
    *written = w;
    return PROVEN_OK;
}

// -----------------------------------------------------------------------------
// Reading challenges
// -----------------------------------------------------------------------------

typedef struct {
    proven_u8str_view_t s;
    proven_size_t pos;
} proven_auth_cur_t;

static void auth_skip_ws(proven_auth_cur_t *c) {
    while (c->pos < c->s.size && (c->s.ptr[c->pos] == ' ' || c->s.ptr[c->pos] == '\t')) c->pos++;
}

static proven_u8str_view_t auth_token(proven_auth_cur_t *c) {
    proven_size_t start = c->pos;
    while (c->pos < c->s.size && auth_is_tchar(c->s.ptr[c->pos])) c->pos++;
    return (proven_u8str_view_t){ .ptr = c->s.ptr + start, .size = c->pos - start };
}

/*
 * One step through a challenge list. A header value looks like
 *
 *     Digest realm="a", nonce="b", Basic realm="c"
 *
 * - scheme names and parameter names are both tokens, told apart by what follows: a parameter
 * is followed by '='. Sets `*scheme` when a new challenge begins (param is then empty), or
 * `*name` and `*value` for a parameter of the current one. Returns false at the end, and sets
 * `*bad` when the text cannot be read.
 */
static bool auth_next(proven_auth_cur_t *c, proven_u8str_view_t *scheme, proven_u8str_view_t *name,
                      proven_u8str_view_t *value, bool *escaped, bool *bad) {
    *scheme = (proven_u8str_view_t){0};
    *name = (proven_u8str_view_t){0};
    *value = (proven_u8str_view_t){0};
    *escaped = false;
    for (;;) {
        auth_skip_ws(c);
        if (c->pos < c->s.size && c->s.ptr[c->pos] == ',') { c->pos++; continue; }
        break;
    }
    if (c->pos >= c->s.size) return false;

    proven_u8str_view_t tok = auth_token(c);
    if (tok.size == 0) { *bad = true; return false; }
    proven_size_t after = c->pos;
    auth_skip_ws(c);
    if (c->pos < c->s.size && c->s.ptr[c->pos] == '=') {
        /* A parameter - unless this is a token68 credential ("Negotiate abc=="), which this
         * function does not need to understand: '=' followed by more '=' or the end. */
        c->pos++;
        auth_skip_ws(c);
        *name = tok;
        if (c->pos < c->s.size && c->s.ptr[c->pos] == '"') {
            c->pos++;
            proven_size_t start = c->pos;
            while (c->pos < c->s.size && c->s.ptr[c->pos] != '"') {
                if (c->s.ptr[c->pos] == '\\') {
                    *escaped = true;
                    c->pos++;
                    if (c->pos >= c->s.size) { *bad = true; return false; }
                }
                c->pos++;
            }
            if (c->pos >= c->s.size) { *bad = true; return false; }
            *value = (proven_u8str_view_t){ .ptr = c->s.ptr + start, .size = c->pos - start };
            c->pos++;
        } else {
            proven_size_t start = c->pos;
            while (c->pos < c->s.size && c->s.ptr[c->pos] != ',' && c->s.ptr[c->pos] != ' ' && c->s.ptr[c->pos] != '\t') c->pos++;
            *value = (proven_u8str_view_t){ .ptr = c->s.ptr + start, .size = c->pos - start };
        }
        return true;
    }
    /* A scheme: a token followed by whitespace and its parameters, or by a comma or the end. */
    c->pos = after;
    *scheme = tok;
    return true;
}

bool proven_http_auth_offers(proven_u8str_view_t header_value, proven_u8str_view_t scheme) {
    if (header_value.size > 0 && !header_value.ptr) return false;
    proven_auth_cur_t c = { .s = header_value, .pos = 0 };
    proven_u8str_view_t sch, name, value;
    bool escaped = false, bad = false;
    while (auth_next(&c, &sch, &name, &value, &escaped, &bad)) {
        if (sch.size > 0 && proven_u8str_view_eq_ascii_ci(sch, scheme)) return true;
    }
    return false;
}

/* Higher is stronger; -1 is an algorithm this library does not implement. */
static int auth_algorithm_rank(proven_u8str_view_t text, proven_http_digest_algorithm_t *out) {
    if (text.size == 0 || auth_eq_nocase(text, "MD5")) { *out = PROVEN_HTTP_DIGEST_MD5; return 1; }
    if (auth_eq_nocase(text, "MD5-sess")) { *out = PROVEN_HTTP_DIGEST_MD5_SESS; return 1; }
    if (auth_eq_nocase(text, "SHA-256")) { *out = PROVEN_HTTP_DIGEST_SHA256; return 2; }
    if (auth_eq_nocase(text, "SHA-256-sess")) { *out = PROVEN_HTTP_DIGEST_SHA256_SESS; return 2; }
    if (auth_eq_nocase(text, "SHA-512-256")) { *out = PROVEN_HTTP_DIGEST_SHA512_256; return 3; }
    if (auth_eq_nocase(text, "SHA-512-256-sess")) { *out = PROVEN_HTTP_DIGEST_SHA512_256_SESS; return 3; }
    return -1;
}

proven_err_t proven_http_digest_challenge_parse(proven_u8str_view_t header_value, proven_http_digest_challenge_t *out) {
    if (!out || (header_value.size > 0 && !header_value.ptr)) return PROVEN_ERR_INVALID_ARG;
    proven_auth_cur_t c = { .s = header_value, .pos = 0 };
    proven_u8str_view_t sch, name, value;
    bool escaped = false, bad = false;

    bool in_digest = false, any_digest = false, any_unsupported = false, have_best = false;
    int best_rank = 0;
    proven_http_digest_challenge_t best = {0};
    proven_http_digest_challenge_t cur = {0};
    proven_u8str_view_t cur_algorithm = {0}, cur_qop = {0};
    bool cur_has_qop = false, cur_escaped = false;

    for (;;) {
        bool more = auth_next(&c, &sch, &name, &value, &escaped, &bad);
        bool boundary = !more || sch.size > 0;
        if (boundary && in_digest) {
            /* The challenge that was being read is complete: judge it. */
            if (cur.realm.ptr == (void *)0 || cur.nonce.size == 0) return PROVEN_ERR_INVALID_FORMAT;
            proven_http_digest_algorithm_t alg = PROVEN_HTTP_DIGEST_MD5;
            int rank = auth_algorithm_rank(cur_algorithm, &alg);
            /* qop absent is the RFC 2069 form; present, it must offer "auth" or "auth-int". */
            bool qop_ok = !cur_has_qop || cur.qop_auth || cur.qop_auth_int;
            if (rank < 0 || !qop_ok || cur_escaped) {
                any_unsupported = true;
            } else if (!have_best || rank > best_rank) {
                cur.algorithm = alg;
                best = cur;
                best_rank = rank;
                have_best = true;
            }
            in_digest = false;
        }
        if (!more) break;
        if (sch.size > 0) {
            in_digest = auth_eq_nocase(sch, "Digest");
            if (in_digest) {
                any_digest = true;
                cur = (proven_http_digest_challenge_t){0};
                cur_algorithm = (proven_u8str_view_t){0};
                cur_qop = (proven_u8str_view_t){0};
                cur_has_qop = false;
                cur_escaped = false;
            }
            continue;
        }
        if (!in_digest) continue;
        if (auth_eq_nocase(name, "realm")) { cur.realm = value; if (!cur.realm.ptr) cur.realm.ptr = header_value.ptr; cur_escaped |= escaped; }
        else if (auth_eq_nocase(name, "nonce")) { cur.nonce = value; cur_escaped |= escaped; }
        else if (auth_eq_nocase(name, "opaque")) { cur.opaque = value; cur.has_opaque = true; cur_escaped |= escaped; }
        else if (auth_eq_nocase(name, "algorithm")) cur_algorithm = value;
        else if (auth_eq_nocase(name, "stale")) cur.stale = auth_eq_nocase(value, "true");
        else if (auth_eq_nocase(name, "userhash")) cur.userhash = auth_eq_nocase(value, "true");
        else if (auth_eq_nocase(name, "charset")) cur.charset_utf8 = auth_eq_nocase(value, "UTF-8");
        else if (auth_eq_nocase(name, "qop")) {
            cur_qop = value;
            cur_has_qop = true;
            /* A list: "auth", "auth-int", or "auth,auth-int". */
            proven_size_t i = 0;
            while (i < cur_qop.size) {
                while (i < cur_qop.size && (cur_qop.ptr[i] == ',' || cur_qop.ptr[i] == ' ' || cur_qop.ptr[i] == '\t')) i++;
                proven_size_t start = i;
                while (i < cur_qop.size && cur_qop.ptr[i] != ',' && cur_qop.ptr[i] != ' ' && cur_qop.ptr[i] != '\t') i++;
                const proven_u8str_view_t item = { .ptr = cur_qop.ptr + start, .size = i - start };
                if (auth_eq_nocase(item, "auth")) cur.qop_auth = true;
                else if (auth_eq_nocase(item, "auth-int")) cur.qop_auth_int = true;
            }
        }
    }
    if (bad) return PROVEN_ERR_INVALID_FORMAT;
    if (have_best) { *out = best; return PROVEN_OK; }
    if (any_unsupported) return PROVEN_ERR_UNSUPPORTED;
    return any_digest ? PROVEN_ERR_INVALID_FORMAT : PROVEN_ERR_NOT_FOUND;
}

// -----------------------------------------------------------------------------
// Answering a Digest challenge
// -----------------------------------------------------------------------------

/* H(data) as lowercase hex, with MD5, SHA-256 or SHA-512/256; returns the number of hex characters. */
enum { AUTH_MD5, AUTH_SHA256, AUTH_SHA512_256 };
typedef struct {
    int kind;
    proven_md5_t md5;
    proven_sha256_t sha;
    proven_sha512_t sha512;
} proven_auth_hash_t;

static void auth_hash_init(proven_auth_hash_t *h, int kind) {
    h->kind = kind;
    if (kind == AUTH_SHA256) proven_sha256_init(&h->sha);
    else if (kind == AUTH_MD5) proven_md5_init(&h->md5);
    else {
        /* SHA-512/256: SHA-512 from the initial values of FIPS 180-4 section 5.3.6.2. */
        static const proven_u64 IV[8] = { 0x22312194FC2BF72Cull, 0x9F555FA3C84C64C2ull, 0x2393B86B6F53B151ull, 0x963877195940EABDull,
                                          0x96283EE2A88EFFE3ull, 0xBE5E1E2553863992ull, 0x2B0199FC2C85B8AAull, 0x0EB72DDC81C52CA2ull };
        proven_sha512_init(&h->sha512);
        for (int i = 0; i < 8; ++i) h->sha512.state[i] = IV[i];
    }
}

static void auth_hash_put(proven_auth_hash_t *h, proven_u8str_view_t s) {
    proven_mem_view_t m = { .ptr = s.ptr, .size = s.size };
    if (h->kind == AUTH_SHA256) proven_sha256_update(&h->sha, m);
    else if (h->kind == AUTH_MD5) proven_md5_update(&h->md5, m);
    else proven_sha512_update(&h->sha512, m);
}

static void auth_hash_colon(proven_auth_hash_t *h) {
    static const proven_byte_t colon = ':';
    auth_hash_put(h, (proven_u8str_view_t){ .ptr = &colon, .size = 1 });
}

static proven_size_t auth_hash_hex(proven_auth_hash_t *h, char out[65]) {
    if (h->kind == AUTH_SHA512_256) {
        proven_byte_t d[PROVEN_SHA512_SIZE];
        proven_sha512_final(&h->sha512, d);
        static const char hexd[] = "0123456789abcdef";
        for (int i = 0; i < 32; ++i) { out[2 * i] = hexd[d[i] >> 4]; out[2 * i + 1] = hexd[d[i] & 15]; }
        out[64] = '\0';
        return 64;
    }
    if (h->kind == AUTH_SHA256) {
        proven_byte_t d[PROVEN_SHA256_SIZE];
        proven_sha256_final(&h->sha, d);
        proven_sha256_to_hex(d, out);
        return 64;
    }
    proven_byte_t d[PROVEN_MD5_SIZE];
    proven_md5_final(&h->md5, d);
    char hex[33];
    proven_md5_to_hex(d, hex);
    for (int i = 0; i < 33; ++i) out[i] = hex[i];
    return 32;
}

typedef struct {
    proven_mem_mut_t out;
    proven_size_t len;
    bool full;
} proven_auth_w_t;

static void auth_put(proven_auth_w_t *w, proven_u8str_view_t s) {
    if (w->full || s.size > w->out.size - w->len) { w->full = true; return; }
    for (proven_size_t i = 0; i < s.size; ++i) w->out.ptr[w->len + i] = s.ptr[i];
    w->len += s.size;
}

static void auth_put_cstr(proven_auth_w_t *w, const char *s) {
    proven_size_t n = 0;
    while (s[n] != '\0') n++;
    auth_put(w, (proven_u8str_view_t){ .ptr = (const proven_byte_t *)s, .size = n });
}

/* RFC 8187's ext-value for `username*`: UTF-8'' and the bytes, attr-char as they are, the rest %XX. */
static void auth_put_ext(proven_auth_w_t *w, proven_u8str_view_t s) {
    static const char hexd[] = "0123456789ABCDEF";
    auth_put_cstr(w, "UTF-8''");
    for (proven_size_t i = 0; i < s.size; ++i) {
        const proven_byte_t ch = s.ptr[i];
        const bool attr = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                          ch == '!' || ch == '#' || ch == '$' || ch == '&' || ch == '+' || ch == '-' || ch == '.' ||
                          ch == '^' || ch == '_' || ch == '`' || ch == '|' || ch == '~';
        if (attr) { auth_put(w, (proven_u8str_view_t){ .ptr = &s.ptr[i], .size = 1 }); continue; }
        const char pct[3] = { '%', hexd[ch >> 4], hexd[ch & 15] };
        auth_put(w, (proven_u8str_view_t){ .ptr = (const proven_byte_t *)pct, .size = 3 });
    }
}


static proven_err_t auth_digest(const proven_http_digest_challenge_t *challenge,
                                proven_u8str_view_t user, proven_u8str_view_t password,
                                proven_u8str_view_t method, proven_u8str_view_t uri,
                                proven_u32 nonce_count, proven_u8str_view_t cnonce, const proven_mem_view_t *body,
                                proven_mem_mut_t out, proven_size_t *written) {
    if (written) *written = 0;
    if (!challenge || !written || (out.size > 0 && !out.ptr) || (body && body->size > 0 && !body->ptr)) return PROVEN_ERR_INVALID_ARG;
    /* The user name is written quoted as it is, hashed (userhash), or as username* (charset=UTF-8). */
    const bool user_plain = auth_quotable(user);
    if ((!user_plain && !challenge->userhash && !challenge->charset_utf8) || auth_has_control(user) ||
        !auth_quotable(challenge->realm) || !auth_quotable(challenge->nonce) ||
        !auth_quotable(challenge->opaque) || !auth_quotable(uri) || !auth_quotable(cnonce) || auth_has_control(password)) {
        return PROVEN_ERR_INVALID_ARG;
    }
    if (method.size == 0 || uri.size == 0 || cnonce.size == 0) return PROVEN_ERR_INVALID_ARG;
    for (proven_size_t i = 0; i < method.size; ++i) if (!auth_is_tchar(method.ptr[i])) return PROVEN_ERR_INVALID_ARG;

    const proven_http_digest_algorithm_t alg = challenge->algorithm;
    const int kind = alg == PROVEN_HTTP_DIGEST_SHA256 || alg == PROVEN_HTTP_DIGEST_SHA256_SESS ? AUTH_SHA256 :
                     alg == PROVEN_HTTP_DIGEST_SHA512_256 || alg == PROVEN_HTTP_DIGEST_SHA512_256_SESS ? AUTH_SHA512_256 : AUTH_MD5;
    const bool sess = alg == PROVEN_HTTP_DIGEST_MD5_SESS || alg == PROVEN_HTTP_DIGEST_SHA256_SESS || alg == PROVEN_HTTP_DIGEST_SHA512_256_SESS;
    /* auth-int when there is a body to cover and the server offers it, or when it offers nothing else. */
    const bool int_qop = challenge->qop_auth_int && (body != (void *)0 || !challenge->qop_auth);
    const bool any_qop = challenge->qop_auth || challenge->qop_auth_int;

    char nc[9];
    static const char hexd[] = "0123456789abcdef";
    for (int i = 0; i < 8; ++i) nc[i] = hexd[(nonce_count >> (28 - 4 * i)) & 0xfu];
    nc[8] = '\0';
    proven_u8str_view_t nc_view = { .ptr = (const proven_byte_t *)nc, .size = 8 };

    /* A1 = user:realm:password, and for the -sess variants H(that):nonce:cnonce. */
    proven_auth_hash_t h;
    char ha1[65], ha2[65], response[65], uh[65], hbody[65];
    auth_hash_init(&h, kind);
    auth_hash_put(&h, user); auth_hash_colon(&h);
    auth_hash_put(&h, challenge->realm); auth_hash_colon(&h);
    auth_hash_put(&h, password);
    proven_size_t hl = auth_hash_hex(&h, ha1);
    if (sess) {
        auth_hash_init(&h, kind);
        auth_hash_put(&h, (proven_u8str_view_t){ .ptr = (const proven_byte_t *)ha1, .size = hl }); auth_hash_colon(&h);
        auth_hash_put(&h, challenge->nonce); auth_hash_colon(&h);
        auth_hash_put(&h, cnonce);
        hl = auth_hash_hex(&h, ha1);
    }
    /* A2 = method:uri, and for auth-int :H(body). */
    if (int_qop) {
        auth_hash_init(&h, kind);
        if (body) auth_hash_put(&h, (proven_u8str_view_t){ .ptr = body->ptr, .size = body->size });
        (void)auth_hash_hex(&h, hbody);
    }
    auth_hash_init(&h, kind);
    auth_hash_put(&h, method); auth_hash_colon(&h);
    auth_hash_put(&h, uri);
    if (int_qop) { auth_hash_colon(&h); auth_hash_put(&h, (proven_u8str_view_t){ .ptr = (const proven_byte_t *)hbody, .size = hl }); }
    (void)auth_hash_hex(&h, ha2);

    /* response = H(HA1:nonce:nc:cnonce:qop:HA2), or H(HA1:nonce:HA2) when the server offered
     * no qop at all (the RFC 2069 form). */
    auth_hash_init(&h, kind);
    auth_hash_put(&h, (proven_u8str_view_t){ .ptr = (const proven_byte_t *)ha1, .size = hl }); auth_hash_colon(&h);
    auth_hash_put(&h, challenge->nonce); auth_hash_colon(&h);
    if (any_qop) {
        auth_hash_put(&h, nc_view); auth_hash_colon(&h);
        auth_hash_put(&h, cnonce); auth_hash_colon(&h);
        auth_hash_put(&h, int_qop ? (proven_u8str_view_t){ .ptr = (const proven_byte_t *)"auth-int", .size = 8 } : (proven_u8str_view_t){ .ptr = (const proven_byte_t *)"auth", .size = 4 });
        auth_hash_colon(&h);
    }
    auth_hash_put(&h, (proven_u8str_view_t){ .ptr = (const proven_byte_t *)ha2, .size = hl });
    (void)auth_hash_hex(&h, response);
    if (challenge->userhash) {
        auth_hash_init(&h, kind);
        auth_hash_put(&h, user); auth_hash_colon(&h);
        auth_hash_put(&h, challenge->realm);
        (void)auth_hash_hex(&h, uh);
    }

    proven_auth_w_t w = { .out = out, .len = 0, .full = false };
    if (challenge->userhash) {
        auth_put_cstr(&w, "Digest username=\"");
        auth_put(&w, (proven_u8str_view_t){ .ptr = (const proven_byte_t *)uh, .size = hl });
        auth_put_cstr(&w, "\"");
    } else if (user_plain) {
        auth_put_cstr(&w, "Digest username=\"");
        auth_put(&w, user);
        auth_put_cstr(&w, "\"");
    } else {
        auth_put_cstr(&w, "Digest username*=");
        auth_put_ext(&w, user);
    }
    auth_put_cstr(&w, ", realm=\"");
    auth_put(&w, challenge->realm);
    auth_put_cstr(&w, "\", nonce=\"");
    auth_put(&w, challenge->nonce);
    auth_put_cstr(&w, "\", uri=\"");
    auth_put(&w, uri);
    auth_put_cstr(&w, "\", algorithm=");
    static const char *const NAMES[6] = { "MD5", "MD5-sess", "SHA-256", "SHA-256-sess", "SHA-512-256", "SHA-512-256-sess" };
    auth_put_cstr(&w, NAMES[(unsigned)alg < 6u ? (unsigned)alg : 0u]);
    auth_put_cstr(&w, ", response=\"");
    auth_put(&w, (proven_u8str_view_t){ .ptr = (const proven_byte_t *)response, .size = hl });
    auth_put_cstr(&w, "\"");
    if (any_qop) {
        auth_put_cstr(&w, int_qop ? ", qop=auth-int, nc=" : ", qop=auth, nc=");
        auth_put(&w, nc_view);
        auth_put_cstr(&w, ", cnonce=\"");
        auth_put(&w, cnonce);
        auth_put_cstr(&w, "\"");
    }
    if (challenge->has_opaque) {
        auth_put_cstr(&w, ", opaque=\"");
        auth_put(&w, challenge->opaque);
        auth_put_cstr(&w, "\"");
    }
    if (challenge->userhash) auth_put_cstr(&w, ", userhash=true");
    if (w.full) return PROVEN_ERR_OUT_OF_BOUNDS;
    *written = w.len;
    return PROVEN_OK;
}

proven_err_t proven_http_digest_auth(const proven_http_digest_challenge_t *challenge,
                                     proven_u8str_view_t user, proven_u8str_view_t password,
                                     proven_u8str_view_t method, proven_u8str_view_t uri,
                                     proven_u32 nonce_count, proven_u8str_view_t cnonce,
                                     proven_mem_mut_t out, proven_size_t *written) {
    return auth_digest(challenge, user, password, method, uri, nonce_count, cnonce, (void *)0, out, written);
}

proven_err_t proven_http_digest_auth_body(const proven_http_digest_challenge_t *challenge,
                                          proven_u8str_view_t user, proven_u8str_view_t password,
                                          proven_u8str_view_t method, proven_u8str_view_t uri,
                                          proven_u32 nonce_count, proven_u8str_view_t cnonce, proven_mem_view_t body,
                                          proven_mem_mut_t out, proven_size_t *written) {
    return auth_digest(challenge, user, password, method, uri, nonce_count, cnonce, &body, out, written);
}
