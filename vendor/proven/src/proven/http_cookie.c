#include "proven/http_cookie.h"
#include "proven/http.h"

/* RFC 6265 section 5, minus domain cookies (see the header). Pure code over an allocator. */

typedef struct {
    proven_byte_t *text;        /* one allocation: name, value, host, path, back to back */
    proven_u32 name_len;
    proven_u32 value_len;
    proven_u32 host_len;
    proven_u32 path_len;
    proven_time_t expires;      /* wall clock; 0: a session cookie */
    proven_u64 stamp;           /* when it was stored, on the jar's counter */
    bool secure;
} proven_cookie_t;

static proven_byte_t ck_lower(proven_byte_t c) { return (c >= 'A' && c <= 'Z') ? (proven_byte_t)(c + 32) : c; }

static bool ck_eq(proven_u8str_view_t a, proven_u8str_view_t b) {
    if (a.size != b.size) return false;
    for (proven_size_t i = 0; i < a.size; ++i) if (a.ptr[i] != b.ptr[i]) return false;
    return true;
}


static bool ck_is(proven_u8str_view_t a, const char *b) {
    proven_size_t n = 0;
    while (b[n] != '\0') n++;
    return proven_u8str_view_eq_ascii_ci(a, (proven_u8str_view_t){ .ptr = (const proven_byte_t *)b, .size = n });
}

static proven_u8str_view_t ck_trim(proven_u8str_view_t s) {
    while (s.size > 0 && (s.ptr[0] == ' ' || s.ptr[0] == '\t')) { s.ptr++; s.size--; }
    while (s.size > 0 && (s.ptr[s.size - 1] == ' ' || s.ptr[s.size - 1] == '\t')) s.size--;
    return s;
}

static proven_u8str_view_t ck_name(const proven_cookie_t *c) { return (proven_u8str_view_t){ .ptr = c->text, .size = c->name_len }; }
static proven_u8str_view_t ck_value(const proven_cookie_t *c) { return (proven_u8str_view_t){ .ptr = c->text + c->name_len, .size = c->value_len }; }
static proven_u8str_view_t ck_host(const proven_cookie_t *c) { return (proven_u8str_view_t){ .ptr = c->text + c->name_len + c->value_len, .size = c->host_len }; }
static proven_u8str_view_t ck_path(const proven_cookie_t *c) {
    return (proven_u8str_view_t){ .ptr = c->text + c->name_len + c->value_len + c->host_len, .size = c->path_len };
}

static proven_cookie_t *ck_entries(const proven_http_cookie_jar_t *jar) { return jar->entries; }

static void ck_remove(proven_http_cookie_jar_t *jar, proven_size_t i) {
    proven_cookie_t *e = ck_entries(jar);
    jar->alloc.free_fn(jar->alloc.ctx, e[i].text);
    e[i] = e[jar->count - 1];
    jar->count--;
}

proven_err_t proven_http_cookie_jar_init(proven_http_cookie_jar_t *jar, proven_allocator_t alloc, proven_size_t max_cookies) {
    if (!jar) return PROVEN_ERR_INVALID_ARG;
    *jar = (proven_http_cookie_jar_t){0};
    if (!proven_alloc_is_valid(alloc) || max_cookies == 0) return PROVEN_ERR_INVALID_ARG;
    if (max_cookies > PROVEN_SIZE_MAX / sizeof(proven_cookie_t)) return PROVEN_ERR_OVERFLOW;
    proven_result_mem_mut_t m = alloc.alloc_fn(alloc.ctx, max_cookies * sizeof(proven_cookie_t), _Alignof(proven_cookie_t));
    if (!proven_is_ok(m.err)) return m.err;
    jar->alloc = alloc;
    jar->entries = m.value.ptr;
    jar->cap = max_cookies;
    return PROVEN_OK;
}

void proven_http_cookie_jar_clear(proven_http_cookie_jar_t *jar) {
    if (!jar || !jar->entries) return;
    while (jar->count > 0) ck_remove(jar, jar->count - 1);
}

void proven_http_cookie_jar_destroy(proven_http_cookie_jar_t *jar) {
    if (!jar || !jar->entries) return;
    proven_http_cookie_jar_clear(jar);
    jar->alloc.free_fn(jar->alloc.ctx, jar->entries);
    *jar = (proven_http_cookie_jar_t){0};
}

proven_size_t proven_http_cookie_jar_count(const proven_http_cookie_jar_t *jar) {
    return jar ? jar->count : 0;
}

/* RFC 6265 section 5.1.3: `host` is `domain` itself, or ends with "." + domain. */
static bool ck_domain_covers(proven_u8str_view_t domain, proven_u8str_view_t host) {
    if (domain.size == 0 || domain.size > host.size) return false;
    proven_u8str_view_t tail = { .ptr = host.ptr + host.size - domain.size, .size = domain.size };
    if (!proven_u8str_view_eq_ascii_ci(tail, domain)) return false;
    return domain.size == host.size || host.ptr[host.size - domain.size - 1] == '.';
}

/* RFC 6265 section 5.1.4. */
static bool ck_path_covers(proven_u8str_view_t cookie_path, proven_u8str_view_t request_path) {
    if (request_path.size == 0) request_path = (proven_u8str_view_t){ .ptr = (const proven_byte_t *)"/", .size = 1 };
    if (cookie_path.size > request_path.size) return false;
    for (proven_size_t i = 0; i < cookie_path.size; ++i) if (cookie_path.ptr[i] != request_path.ptr[i]) return false;
    if (cookie_path.size == request_path.size) return true;
    return cookie_path.ptr[cookie_path.size - 1] == '/' || request_path.ptr[cookie_path.size] == '/';
}

/* The default path: the request path up to, not including, its last '/'; "/" when that is
 * empty or the path is not absolute. */
static proven_u8str_view_t ck_default_path(proven_u8str_view_t request_path) {
    static const proven_byte_t root = '/';
    proven_u8str_view_t slash = { .ptr = &root, .size = 1 };
    if (request_path.size == 0 || request_path.ptr[0] != '/') return slash;
    proven_size_t last = 0;
    for (proven_size_t i = 0; i < request_path.size; ++i) if (request_path.ptr[i] == '/') last = i;
    if (last == 0) return slash;
    return (proven_u8str_view_t){ .ptr = request_path.ptr, .size = last };
}

proven_err_t proven_http_cookie_jar_store(proven_http_cookie_jar_t *jar, proven_u8str_view_t host, proven_u8str_view_t path,
                                          bool secure, proven_u8str_view_t set_cookie, proven_time_t now_wall) {
    if (!jar || !jar->entries || host.size == 0 || !host.ptr || (set_cookie.size > 0 && !set_cookie.ptr)) return PROVEN_ERR_INVALID_ARG;
    for (proven_size_t i = 0; i < set_cookie.size; ++i) {
        proven_byte_t c = set_cookie.ptr[i];
        if ((c < 0x20 && c != '\t') || c == 0x7f) return PROVEN_ERR_INVALID_FORMAT;
    }

    /* name=value, then attributes separated by ';'. */
    proven_size_t semi = 0;
    while (semi < set_cookie.size && set_cookie.ptr[semi] != ';') semi++;
    proven_u8str_view_t pair = { .ptr = set_cookie.ptr, .size = semi };
    proven_size_t eq = 0;
    while (eq < pair.size && pair.ptr[eq] != '=') eq++;
    if (eq == pair.size) return PROVEN_ERR_INVALID_FORMAT;
    proven_u8str_view_t name = ck_trim((proven_u8str_view_t){ .ptr = pair.ptr, .size = eq });
    proven_u8str_view_t value = ck_trim((proven_u8str_view_t){ .ptr = pair.ptr + eq + 1, .size = pair.size - eq - 1 });
    if (name.size == 0) return PROVEN_ERR_INVALID_FORMAT;
    if (name.size + value.size > PROVEN_HTTP_COOKIE_MAX_SIZE) return PROVEN_ERR_OUT_OF_BOUNDS;

    proven_u8str_view_t cookie_path = ck_default_path(path);
    proven_time_t expires = 0;
    bool has_max_age = false, is_secure = false, expired = false;

    proven_size_t pos = semi;
    while (pos < set_cookie.size) {
        pos++;                                  /* the ';' */
        proven_size_t end = pos;
        while (end < set_cookie.size && set_cookie.ptr[end] != ';') end++;
        proven_u8str_view_t attr = { .ptr = set_cookie.ptr + pos, .size = end - pos };
        proven_size_t aeq = 0;
        while (aeq < attr.size && attr.ptr[aeq] != '=') aeq++;
        proven_u8str_view_t an = ck_trim((proven_u8str_view_t){ .ptr = attr.ptr, .size = aeq });
        proven_u8str_view_t av = aeq < attr.size ? ck_trim((proven_u8str_view_t){ .ptr = attr.ptr + aeq + 1, .size = attr.size - aeq - 1 })
                                                : (proven_u8str_view_t){ .ptr = attr.ptr + attr.size, .size = 0 };
        pos = end;

        if (ck_is(an, "Secure")) {
            is_secure = true;
        } else if (ck_is(an, "Path")) {
            if (av.size > 0 && av.ptr[0] == '/') cookie_path = av;
        } else if (ck_is(an, "Domain")) {
            if (av.size > 0 && av.ptr[0] == '.') { av.ptr++; av.size--; }
            /* Accepted when it names this host or a parent of it - and then ignored: the
             * cookie stays host-only. Anything else is a server setting a cookie for a site
             * it is not. */
            if (av.size > 0 && !ck_domain_covers(av, host)) return PROVEN_ERR_PERMISSION;
        } else if (ck_is(an, "Max-Age")) {
            /* Digits with an optional minus sign; anything else, the attribute is ignored.
             * Max-Age wins over Expires whichever comes first. */
            proven_size_t i = 0;
            bool negative = av.size > 0 && av.ptr[0] == '-';
            if (negative) i = 1;
            proven_i64 seconds = 0;
            bool ok = i < av.size;
            for (; i < av.size && ok; ++i) {
                if (av.ptr[i] < '0' || av.ptr[i] > '9') ok = false;
                else if (seconds < 4000000000LL) seconds = seconds * 10 + (av.ptr[i] - '0');
            }
            if (ok) {
                has_max_age = true;
                if (negative || seconds == 0) { expired = true; expires = 0; }
                else {
                    expired = false;
                    /* Far enough ahead to pass the end of the clock simply never expires. */
                    proven_i64 limit = (INT64_MAX - now_wall) / 1000000000;
                    expires = seconds >= limit ? INT64_MAX : now_wall + seconds * 1000000000;
                }
            }
        } else if (ck_is(an, "Expires") && !has_max_age) {
            proven_time_t when = 0;
            proven_err_t e = proven_http_date_parse(av, now_wall, &when);
            if (e == PROVEN_OK) {
                expires = when;
                expired = when <= now_wall;
                if (when == 0) expired = true;
            } else if (e == PROVEN_ERR_OVERFLOW) {
                expires = INT64_MAX;             /* later than the clock can say: never */
                expired = false;
            }
            /* A date that cannot be read is ignored, and the cookie is a session cookie. */
        }
    }
    if (is_secure && !secure) return PROVEN_ERR_PERMISSION;

    /* An existing cookie of the same name, host and path is replaced - or just removed. */
    proven_cookie_t *e = ck_entries(jar);
    proven_size_t same = jar->count;
    for (proven_size_t i = 0; i < jar->count; ++i) {
        if (ck_eq(ck_name(&e[i]), name) && proven_u8str_view_eq_ascii_ci(ck_host(&e[i]), host) && ck_eq(ck_path(&e[i]), cookie_path)) { same = i; break; }
    }
    if (expired) {
        if (same < jar->count) ck_remove(jar, same);
        return PROVEN_OK;
    }

    proven_size_t total = name.size + value.size + host.size + cookie_path.size;
    proven_result_mem_mut_t m = jar->alloc.alloc_fn(jar->alloc.ctx, total ? total : 1, 1);
    if (!proven_is_ok(m.err)) return m.err;
    proven_byte_t *t = m.value.ptr;
    proven_size_t w = 0;
    for (proven_size_t i = 0; i < name.size; ++i) t[w++] = name.ptr[i];
    for (proven_size_t i = 0; i < value.size; ++i) t[w++] = value.ptr[i];
    for (proven_size_t i = 0; i < host.size; ++i) t[w++] = ck_lower(host.ptr[i]);
    for (proven_size_t i = 0; i < cookie_path.size; ++i) t[w++] = cookie_path.ptr[i];

    if (same < jar->count) {
        ck_remove(jar, same);
    } else if (jar->count == jar->cap) {
        proven_size_t oldest = 0;
        for (proven_size_t i = 1; i < jar->count; ++i) if (e[i].stamp < e[oldest].stamp) oldest = i;
        ck_remove(jar, oldest);
    }
    proven_cookie_t *slot = &e[jar->count++];
    slot->text = t;
    slot->name_len = (proven_u32)name.size;
    slot->value_len = (proven_u32)value.size;
    slot->host_len = (proven_u32)host.size;
    slot->path_len = (proven_u32)cookie_path.size;
    slot->expires = expires;
    slot->secure = is_secure;
    slot->stamp = ++jar->clock;
    return PROVEN_OK;
}

proven_err_t proven_http_cookie_jar_header(proven_http_cookie_jar_t *jar, proven_u8str_view_t host, proven_u8str_view_t path,
                                           bool secure, proven_time_t now_wall,
                                           proven_mem_mut_t out, proven_size_t *written) {
    if (written) *written = 0;
    if (!jar || !jar->entries || !written || (out.size > 0 && !out.ptr)) return PROVEN_ERR_INVALID_ARG;
    proven_cookie_t *e = ck_entries(jar);

    for (proven_size_t i = 0; i < jar->count;) {
        if (e[i].expires != 0 && e[i].expires <= now_wall) ck_remove(jar, i);
        else i++;
    }

    /* Longest path first, then oldest first: repeatedly take the best cookie not yet written.
     * The jar is small and bounded, so a selection pass per cookie is cheaper than a sort that
     * would need memory. `taken` marks by stamp: every stamp is unique. */
    proven_size_t w = 0;
    proven_u64 last_stamp = 0;
    proven_size_t last_path = PROVEN_SIZE_MAX;
    for (;;) {
        proven_size_t best = jar->count;
        for (proven_size_t i = 0; i < jar->count; ++i) {
            if (!proven_u8str_view_eq_ascii_ci(ck_host(&e[i]), host) || !ck_path_covers(ck_path(&e[i]), path)) continue;
            if (e[i].secure && !secure) continue;
            /* Already written: anything ordered at or before the last one taken. */
            bool before_last = e[i].path_len > last_path || (e[i].path_len == last_path && e[i].stamp <= last_stamp);
            if (last_path != PROVEN_SIZE_MAX && before_last) continue;
            if (best == jar->count || e[i].path_len > e[best].path_len ||
                (e[i].path_len == e[best].path_len && e[i].stamp < e[best].stamp)) best = i;
        }
        if (best == jar->count) break;
        proven_u8str_view_t n = ck_name(&e[best]), v = ck_value(&e[best]);
        proven_size_t need = n.size + 1 + v.size + (w > 0 ? 2 : 0);
        if (need > out.size - w) return PROVEN_ERR_OUT_OF_BOUNDS;
        if (w > 0) { out.ptr[w++] = ';'; out.ptr[w++] = ' '; }
        for (proven_size_t i = 0; i < n.size; ++i) out.ptr[w++] = n.ptr[i];
        out.ptr[w++] = '=';
        for (proven_size_t i = 0; i < v.size; ++i) out.ptr[w++] = v.ptr[i];
        last_stamp = e[best].stamp;
        last_path = e[best].path_len;
    }
    *written = w;
    return PROVEN_OK;
}
