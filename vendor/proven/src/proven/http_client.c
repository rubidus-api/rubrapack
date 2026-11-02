#include "proven/http_client.h"

#if !defined(PROVEN_FREESTANDING) && !defined(PROVEN_NO_NET)

#include "proven/url.h"
#include "proven/http_auth.h"
#include "proven/random.h"
#include "proven/time.h"
#include "proven_internal_http_coding.h"
#include "proven_internal_tls.h"
#if !defined(PROVEN_NO_HTTP3)
#include "proven/loop.h"
#include "proven/http_event_client.h"
#endif

/*
 * The HTTP/1.1 client driver. The message format is http.c's business and the sockets are
 * net.c's; what is here is the sequence between them: find or make a connection, write a
 * request, read a head, decide whether that head is the answer or a reason to send another
 * request, and hand the body out through a decoder.
 *
 * A request asked over HTTP/3 (RFC-0034) has an exchange of its own: the event-driven client on
 * a loop of this client's, made at the first such request, which the caller's thread runs while
 * it waits - for the head in send, for body bytes in read. The head is copied into the call as an
 * HTTP/1.1 head would sit there, the body comes through a queue (the request paused while it is
 * full), and everything above the exchange - redirects, challenges, cookies, decoding - is the same.
 */

#define CL_H3_QUEUE_HIGH 65536u          /* body bytes queued before the request is paused */
#define CL_H3_QUEUE_LOW 16384u           /* ... and below which it goes on */

#define CL_DEFAULT_TIMEOUT_MS 30000u
#define CL_MAX_HEADERS 64
#define CL_URL_MAX 2048
#define CL_DRAIN_LIMIT 65536u         /* a body larger than this is not read just to keep a connection */
#define CL_MAX_ADDRS 8
#define CL_MAX_INTERIM 8                /* 1xx responses passed over before one is taken for an attack */

/* One connection: the socket, and the transport the exchange runs over (the socket itself, or
 * what tls_wrap built on it). `key` says where it leads, so that it is reused only for that. */
typedef struct cl_conn {
    proven_net_conn_t sock;
    proven_transport_t transport;
    bool wrapped;                      /* transport closes the socket itself */
    bool via_http_proxy;               /* requests on it use the absolute form */
    proven_allocator_t alloc;          /* set when an upgrade hands the connection to the caller */
    proven_u64 last_used;
    proven_size_t key_len;
    proven_byte_t key[300];            /* "scheme://host:port" */
} proven_cl_conn_t;

struct proven_http_client {
    proven_http_client_config_t cfg;   /* string views point into `strings` */
#if !defined(PROVEN_NO_HTTP3)
    proven_loop_t *loop;               /* HTTP/3 (RFC-0034): a loop and an event client of its own, made when first needed */
    proven_http_event_client_t *ev;
#endif
    proven_byte_t *strings;
    proven_url_t proxy;
    bool has_proxy;
    bool proxy_socks;
    proven_cl_conn_t **idle;
    proven_size_t idle_count;
    proven_u64 tick;
};

/* Everything one send needs, in one allocation. */
typedef struct {
    proven_http_client_t *client;
    proven_cl_conn_t *conn;
    proven_byte_t *buf;                /* response head, then the body read window */
    proven_size_t buf_cap;
    proven_size_t buf_len;
    proven_size_t buf_pos;
    proven_byte_t *out;                /* the request head being built */
    proven_size_t out_cap;
    proven_byte_t url[CL_URL_MAX];
    proven_size_t url_len;
    proven_http_header_t headers[CL_MAX_HEADERS];
    proven_http_response_t res;
    proven_http_body_t body;
    proven_byte_t trailers[PROVEN_HTTP_MAX_TRAILER_BYTES];   /* a chunked response's trailer section (proven_http_client_trailers) */
    bool body_done;
    bool keep_alive;
    bool reused;
    bool got_response_bytes;
    bool decoding;                     /* the body is decoded on its way to the caller */
    proven_mem_view_t pending;         /* decoding: encoded bytes of the window not decoded yet */
    proven_http_decoder_t_ dec;
#if !defined(PROVEN_NO_HTTP3)
    /* HTTP/3 (RFC-0034): the event client's request, what it has said, the body queue, and a
     * stream body being written. */
    bool h3;
    proven_http_event_request_t *h3_req;
    bool h3_head, h3_done;
    proven_err_t h3_why;
    proven_byte_t *h3q;
    proven_size_t h3q_len, h3q_pos, h3q_cap;
    bool h3_paused;
    proven_reader_t h3_stream;
    proven_size_t h3_pend_off, h3_pend_len;
    bool h3_stream_end;
#endif
} proven_cl_call_t;

static proven_u8str_view_t cl_lit(const char *s) {
    proven_size_t n = 0;
    while (s[n] != '\0') n++;
    return (proven_u8str_view_t){ .ptr = (const proven_byte_t *)s, .size = n };
}

static proven_byte_t cl_lower(proven_byte_t c) { return (c >= 'A' && c <= 'Z') ? (proven_byte_t)(c + 32) : c; }


static bool cl_has_control(proven_u8str_view_t s) {
    for (proven_size_t i = 0; i < s.size; ++i) if (s.ptr[i] < 0x20 || s.ptr[i] == 0x7f) return true;
    return false;
}

static void *cl_alloc(proven_allocator_t a, proven_size_t size) {
    proven_result_mem_mut_t m = a.alloc_fn(a.ctx, size ? size : 1, 16);
    return proven_is_ok(m.err) ? m.value.ptr : (void *)0;
}

static void cl_free(proven_allocator_t a, void *p) {
    if (p) a.free_fn(a.ctx, p);
}

static proven_net_deadline_t cl_io_deadline(const proven_http_client_t *c) {
    return proven_net_deadline_in(c->cfg.io_timeout_ms);
}

// -----------------------------------------------------------------------------
// The client object
// -----------------------------------------------------------------------------

proven_err_t proven_http_client_create(const proven_http_client_config_t *config, proven_http_client_t **out) {
    if (out) *out = (void *)0;
    if (!config || !out || !proven_alloc_is_valid(config->alloc)) return PROVEN_ERR_INVALID_ARG;
    if (cl_has_control(config->username) || cl_has_control(config->password) || cl_has_control(config->user_agent)) return PROVEN_ERR_INVALID_ARG;
    const proven_err_t coding_e = proven_http_decoder_cfg_check_(config->decompress, config->decompress_zstd, config->decompress_brotli);
    if (coding_e != PROVEN_OK) return coding_e;
    if (config->http3) {
        /* HTTP/3 needs its own TLS configuration, one that offers h3 (RFC-0034). */
#if defined(PROVEN_NO_HTTP3)
        return PROVEN_ERR_UNSUPPORTED;
#else
        bool offers_h3 = false;
        for (proven_size_t i = 0; config->tls; ++i) {
            const proven_u8str_view_t name = proven_tls_config_alpn(config->tls, i);
            if (name.size == 0) break;
            if (name.size == 2 && name.ptr[0] == 'h' && name.ptr[1] == '3') offers_h3 = true;
        }
        if (!offers_h3) return PROVEN_ERR_INVALID_ARG;
#endif
    } else if (config->http3_fallback) {
        return PROVEN_ERR_INVALID_ARG;
    }

    proven_http_client_t *c = cl_alloc(config->alloc, sizeof *c);
    if (!c) return PROVEN_ERR_NOMEM;
    *c = (proven_http_client_t){0};
    c->cfg = *config;
    if (c->cfg.connect_timeout_ms == 0) c->cfg.connect_timeout_ms = CL_DEFAULT_TIMEOUT_MS;
    if (c->cfg.io_timeout_ms == 0) c->cfg.io_timeout_ms = CL_DEFAULT_TIMEOUT_MS;
    if (c->cfg.max_head_bytes == 0) c->cfg.max_head_bytes = PROVEN_HTTP_DEFAULT_MAX_HEAD;

    if (c->cfg.expect_timeout_ms == 0) c->cfg.expect_timeout_ms = 1000;

    /* The five strings are copied, so the caller's need not outlive this call. */
    proven_size_t total = config->proxy.size + config->username.size + config->password.size + config->user_agent.size + config->no_proxy.size;
    c->strings = cl_alloc(config->alloc, total);
    if (!c->strings) { cl_free(config->alloc, c); return PROVEN_ERR_NOMEM; }
    proven_size_t at = 0;
    proven_u8str_view_t *dst[5] = { &c->cfg.proxy, &c->cfg.username, &c->cfg.password, &c->cfg.user_agent, &c->cfg.no_proxy };
    const proven_u8str_view_t src[5] = { config->proxy, config->username, config->password, config->user_agent, config->no_proxy };
    for (int i = 0; i < 5; ++i) {
        for (proven_size_t k = 0; k < src[i].size; ++k) c->strings[at + k] = src[i].ptr[k];
        *dst[i] = (proven_u8str_view_t){ .ptr = c->strings + at, .size = src[i].size };
        at += src[i].size;
    }

    if (c->cfg.proxy.size > 0) {
        bool ok = proven_url_parse(c->cfg.proxy, &c->proxy) == PROVEN_OK && c->proxy.host.size > 0;
        bool http = ok && proven_url_scheme_is(&c->proxy, cl_lit("http"));
        bool socks = ok && (proven_url_scheme_is(&c->proxy, cl_lit("socks5")) || proven_url_scheme_is(&c->proxy, cl_lit("socks5h")));
        if (!http && !socks) {
            cl_free(config->alloc, c->strings);
            cl_free(config->alloc, c);
            return PROVEN_ERR_INVALID_ARG;
        }
        c->has_proxy = true;
        c->proxy_socks = socks;
    }
    if (c->cfg.max_idle_connections > 0) {
        if (c->cfg.max_idle_connections > PROVEN_SIZE_MAX / sizeof(proven_cl_conn_t *)) c->cfg.max_idle_connections = 64;
        c->idle = cl_alloc(config->alloc, c->cfg.max_idle_connections * sizeof(proven_cl_conn_t *));
        if (!c->idle) {
            cl_free(config->alloc, c->strings);
            cl_free(config->alloc, c);
            return PROVEN_ERR_NOMEM;
        }
    }
    *out = c;
    return PROVEN_OK;
}

static void cl_conn_close(proven_http_client_t *c, proven_cl_conn_t *conn) {
    if (!conn) return;
    if (conn->wrapped) (void)proven_transport_close(conn->transport);
    (void)proven_net_close(&conn->sock);
    cl_free(c->cfg.alloc, conn);
}

void proven_http_client_destroy(proven_http_client_t *client) {
    if (!client) return;
#if !defined(PROVEN_NO_HTTP3)
    if (client->ev) {
        /* Its QUIC connections closed, and their sockets let go by the loop before it goes. */
        proven_http_event_client_destroy(client->ev);
        const proven_net_deadline_t until = proven_net_deadline_in(200);
        while (proven_time_monotonic_now() < until) (void)proven_loop_poll(client->loop, proven_net_deadline_in(10));
    }
    if (client->loop) proven_loop_destroy(client->loop);
#endif
    for (proven_size_t i = 0; i < client->idle_count; ++i) cl_conn_close(client, client->idle[i]);
    proven_allocator_t a = client->cfg.alloc;
    cl_free(a, client->idle);
    cl_free(a, client->strings);
    cl_free(a, client);
}

// -----------------------------------------------------------------------------
// Connections
// -----------------------------------------------------------------------------

static proven_cl_conn_t *cl_pool_take(proven_http_client_t *c, proven_u8str_view_t key) {
    for (proven_size_t i = 0; i < c->idle_count; ++i) {
        proven_cl_conn_t *k = c->idle[i];
        if (k->key_len == key.size && proven_memcmp(k->key, key.ptr, key.size) == 0) {
            c->idle[i] = c->idle[--c->idle_count];
            return k;
        }
    }
    return (void *)0;
}

static void cl_pool_put(proven_http_client_t *c, proven_cl_conn_t *conn) {
    if (c->cfg.max_idle_connections == 0) { cl_conn_close(c, conn); return; }
    if (c->idle_count == c->cfg.max_idle_connections) {
        proven_size_t oldest = 0;
        for (proven_size_t i = 1; i < c->idle_count; ++i) if (c->idle[i]->last_used < c->idle[oldest]->last_used) oldest = i;
        cl_conn_close(c, c->idle[oldest]);
        c->idle[oldest] = c->idle[--c->idle_count];
    }
    conn->last_used = ++c->tick;
    c->idle[c->idle_count++] = conn;
}

/* Read exactly `n` bytes; a peer that closes first is a reset. */
static proven_err_t cl_read_exact(proven_transport_t t, proven_byte_t *dst, proven_size_t n, proven_net_deadline_t until) {
    proven_size_t got = 0;
    while (got < n) {
        proven_result_size_t r = proven_transport_read(t, (proven_mem_mut_t){ .ptr = dst + got, .size = n - got }, until);
        if (r.err == PROVEN_ERR_EOF) return PROVEN_ERR_RESET;
        if (r.err != PROVEN_OK) return r.err;
        got += r.value;
    }
    return PROVEN_OK;
}

/* The proxy URL's user and password, percent-decoded as RFC 3986 has them (`p%40ss` is `p@ss`),
 * into `ub` and `pb` (each at least as long as the userinfo); left as written when they do not decode. */
static void cl_proxy_creds(const proven_http_client_t *c, proven_byte_t *ub, proven_byte_t *pb, proven_size_t cap,
                           proven_u8str_view_t *user, proven_u8str_view_t *pass) {
    proven_u8str_view_t ui = c->proxy.userinfo;
    *user = ui;
    *pass = (proven_u8str_view_t){ .ptr = ui.ptr + ui.size, .size = 0 };
    for (proven_size_t i = 0; i < ui.size; ++i) {
        if (ui.ptr[i] == ':') {
            *user = (proven_u8str_view_t){ .ptr = ui.ptr, .size = i };
            *pass = (proven_u8str_view_t){ .ptr = ui.ptr + i + 1, .size = ui.size - i - 1 };
            break;
        }
    }
    proven_size_t n = 0;
    if (proven_url_percent_decode(*user, (proven_mem_mut_t){ .ptr = ub, .size = cap }, &n) == PROVEN_OK) *user = (proven_u8str_view_t){ .ptr = ub, .size = n };
    if (proven_url_percent_decode(*pass, (proven_mem_mut_t){ .ptr = pb, .size = cap }, &n) == PROVEN_OK) *pass = (proven_u8str_view_t){ .ptr = pb, .size = n };
}

/* RFC 1928 and RFC 1929: ask a SOCKS5 proxy to connect to host:port. The name is sent as a
 * name, so it is the proxy that resolves it. */
static proven_err_t cl_socks5(proven_http_client_t *c, proven_transport_t t, proven_u8str_view_t host, proven_u16 port, proven_net_deadline_t until) {
    if (host.size == 0 || host.size > 255) return PROVEN_ERR_INVALID_ARG;
    proven_byte_t m[600];
    proven_size_t n = 0;
    bool creds = c->proxy.has_userinfo;
    proven_byte_t ub[CL_URL_MAX], pb[CL_URL_MAX];
    proven_u8str_view_t user, pass;
    cl_proxy_creds(c, ub, pb, sizeof ub, &user, &pass);
    if (user.size > 255 || pass.size > 255) return PROVEN_ERR_INVALID_ARG;

    m[n++] = 5;
    m[n++] = creds ? 2 : 1;
    m[n++] = 0;                                   /* no authentication */
    if (creds) m[n++] = 2;                        /* user name and password */
    proven_result_size_t w = proven_transport_write_all(t, (proven_mem_view_t){ .ptr = m, .size = n }, until);
    if (w.err != PROVEN_OK) return w.err;
    proven_err_t e = cl_read_exact(t, m, 2, until);
    if (e != PROVEN_OK) return e;
    if (m[0] != 5) return PROVEN_ERR_INVALID_FORMAT;
    if (m[1] == 2 && creds) {
        n = 0;
        m[n++] = 1;
        m[n++] = (proven_byte_t)user.size;
        for (proven_size_t i = 0; i < user.size; ++i) m[n++] = user.ptr[i];
        m[n++] = (proven_byte_t)pass.size;
        for (proven_size_t i = 0; i < pass.size; ++i) m[n++] = pass.ptr[i];
        w = proven_transport_write_all(t, (proven_mem_view_t){ .ptr = m, .size = n }, until);
        if (w.err != PROVEN_OK) return w.err;
        e = cl_read_exact(t, m, 2, until);
        if (e != PROVEN_OK) return e;
        if (m[1] != 0) return PROVEN_ERR_PERMISSION;
    } else if (m[1] != 0) {
        return PROVEN_ERR_PERMISSION;             /* 0xFF: none of the offered methods is acceptable */
    }

    n = 0;
    m[n++] = 5; m[n++] = 1; m[n++] = 0;           /* CONNECT */
    m[n++] = 3;                                   /* a domain name follows */
    m[n++] = (proven_byte_t)host.size;
    for (proven_size_t i = 0; i < host.size; ++i) m[n++] = host.ptr[i];
    m[n++] = (proven_byte_t)(port >> 8);
    m[n++] = (proven_byte_t)(port & 0xff);
    w = proven_transport_write_all(t, (proven_mem_view_t){ .ptr = m, .size = n }, until);
    if (w.err != PROVEN_OK) return w.err;

    e = cl_read_exact(t, m, 4, until);
    if (e != PROVEN_OK) return e;
    if (m[0] != 5) return PROVEN_ERR_INVALID_FORMAT;
    proven_byte_t rep = m[1], atyp = m[3];
    /* The bound address follows and is of no use here, but it must be read off the stream. */
    proven_size_t rest = 0;
    if (atyp == 1) rest = 4 + 2;
    else if (atyp == 4) rest = 16 + 2;
    else if (atyp == 3) {
        e = cl_read_exact(t, m, 1, until);
        if (e != PROVEN_OK) return e;
        rest = (proven_size_t)m[0] + 2;
    } else return PROVEN_ERR_INVALID_FORMAT;
    e = cl_read_exact(t, m, rest, until);
    if (e != PROVEN_OK) return e;
    switch (rep) {
        case 0: return PROVEN_OK;
        case 2: return PROVEN_ERR_PERMISSION;     /* not allowed by the rule set */
        case 3:
        case 4: return PROVEN_ERR_UNREACHABLE;
        case 5: return PROVEN_ERR_REFUSED;
        case 6: return PROVEN_ERR_TIMEOUT;
        default: return PROVEN_ERR_IO;
    }
}

static void cl_random_hex(proven_byte_t out[32]);


/* The answer to a proxy's Digest challenge among `headers` (Proxy-Authenticate), for `method` and
 * `uri` as they are sent; false when there is none this library can answer. */
static bool cl_proxy_digest(const proven_http_client_t *c, const proven_http_header_t *headers, proven_size_t count,
                            proven_u8str_view_t method, proven_u8str_view_t uri, proven_byte_t *out, proven_size_t cap, proven_size_t *len) {
    if (!c->proxy.has_userinfo) return false;
    proven_byte_t ub[CL_URL_MAX], pb[CL_URL_MAX];
    proven_u8str_view_t user, pass;
    cl_proxy_creds(c, ub, pb, sizeof ub, &user, &pass);
    for (proven_size_t i = 0; i < count; ++i) {
        if (!proven_u8str_view_eq_ascii_ci(headers[i].name, cl_lit("Proxy-Authenticate"))) continue;
        proven_http_digest_challenge_t dc;
        if (proven_http_digest_challenge_parse(headers[i].value, &dc) != PROVEN_OK) continue;
        proven_byte_t cnonce[32];
        cl_random_hex(cnonce);
        if (proven_http_digest_auth(&dc, user, pass, method, uri, 1, (proven_u8str_view_t){ .ptr = cnonce, .size = sizeof cnonce },
                                    (proven_mem_mut_t){ .ptr = out, .size = cap }, len) == PROVEN_OK) return true;
    }
    return false;
}

/* "Proxy-Authorization: Basic ..." from the proxy URL's userinfo, appended to a head. */
static proven_err_t cl_proxy_auth(proven_http_client_t *c, proven_mem_mut_t out, proven_size_t *len) {
    if (!c->proxy.has_userinfo) return PROVEN_OK;
    proven_byte_t ub[CL_URL_MAX], pb[CL_URL_MAX];
    proven_u8str_view_t user, pass;
    cl_proxy_creds(c, ub, pb, sizeof ub, &user, &pass);
    proven_byte_t value[400];
    proven_size_t n = 0;
    proven_err_t e = proven_http_basic_auth(user, pass, (proven_mem_mut_t){ .ptr = value, .size = sizeof value }, &n);
    if (e != PROVEN_OK) return e;
    return proven_http_write_header(out, len, cl_lit("Proxy-Authorization"), (proven_u8str_view_t){ .ptr = value, .size = n });
}

/* Ask an HTTP proxy for a tunnel to host:port (RFC 9110 section 9.3.6). */
static proven_err_t cl_http_tunnel(proven_http_client_t *c, proven_transport_t t, proven_u8str_view_t authority, proven_net_deadline_t until) {
    proven_byte_t m[1024];
    proven_byte_t digest[1024];
    proven_size_t digest_len = 0;
    /* Twice at most: with the userinfo as Basic, and once more answering a 407's Digest challenge
     * (RFC 7616: the method CONNECT, the authority as the uri). */
    for (int round = 0; round < 2; ++round) {
        proven_mem_mut_t out = { .ptr = m, .size = sizeof m };
        proven_size_t len = 0;
        proven_err_t e = proven_http_write_request_line(out, &len, cl_lit("CONNECT"), authority);
        if (e == PROVEN_OK) e = proven_http_write_header(out, &len, cl_lit("Host"), authority);
        if (e == PROVEN_OK) e = round == 0 ? cl_proxy_auth(c, out, &len)
                                           : proven_http_write_header(out, &len, cl_lit("Proxy-Authorization"), (proven_u8str_view_t){ .ptr = digest, .size = digest_len });
        if (e == PROVEN_OK) e = proven_http_write_head_end(out, &len);
        if (e != PROVEN_OK) return e;
        proven_result_size_t w = proven_transport_write_all(t, (proven_mem_view_t){ .ptr = m, .size = len }, until);
        if (w.err != PROVEN_OK) return w.err;

        /* The reply is read a byte at a time: whatever follows its head already belongs to the
         * tunnel, and must not be taken out of it here. A CONNECT reply is a few dozen bytes. */
        proven_size_t have = 0;
        for (;;) {
            if (have == sizeof m) return PROVEN_ERR_OUT_OF_BOUNDS;
            e = cl_read_exact(t, m + have, 1, until);
            if (e != PROVEN_OK) return e;
            have++;
            proven_http_header_t h[16];
            proven_http_response_t res;
            proven_size_t head = 0;
            e = proven_http_parse_response((proven_mem_view_t){ .ptr = m, .size = have }, h, 16, sizeof m, &res, &head);
            if (e == PROVEN_ERR_NEED_MORE) continue;
            if (e != PROVEN_OK) return e;
            if (res.status >= 200 && res.status < 300) return PROVEN_OK;
            if (res.status != 407 || round == 1 ||
                !cl_proxy_digest(c, h, res.header_count, cl_lit("CONNECT"), authority, digest, sizeof digest, &digest_len)) {
                return res.status == 407 || res.status == 403 ? PROVEN_ERR_PERMISSION : PROVEN_ERR_REFUSED;
            }
            /* The 407's body is read away so that the connection can carry the next CONNECT: it
             * must have a length, and be short; a proxy that closes is not asked again. */
            proven_http_framing_t f;
            if (proven_http_response_framing(&res, PROVEN_HTTP_CONNECT, &f) != PROVEN_OK || (f.kind != PROVEN_HTTP_BODY_LENGTH && f.kind != PROVEN_HTTP_BODY_NONE) ||
                f.length > sizeof m || !proven_http_response_keep_alive(&res)) return PROVEN_ERR_PERMISSION;
            if (f.kind == PROVEN_HTTP_BODY_LENGTH && f.length > 0 && cl_read_exact(t, m, (proven_size_t)f.length, until) != PROVEN_OK) return PROVEN_ERR_PERMISSION;
            break;
        }
    }
    return PROVEN_ERR_PERMISSION;
}

/* Connect a socket to host:port, trying each address the name has until one answers. */
static proven_err_t cl_dial(proven_http_client_t *c, proven_u8str_view_t host, proven_u16 port, proven_net_deadline_t until, proven_net_conn_t *out) {
    proven_net_addr_t addrs[CL_MAX_ADDRS];
    proven_size_t count = 0;
    (void)c;
    proven_err_t e = proven_net_resolve(host, port, addrs, CL_MAX_ADDRS, &count);
    if (e != PROVEN_OK) return e;
    proven_err_t last = PROVEN_ERR_NOT_FOUND;
    for (proven_size_t i = 0; i < count; ++i) {
        last = proven_net_connect(addrs[i], until, out);
        if (last == PROVEN_OK) {
            (void)proven_net_conn_set_nodelay(out, true);
            return PROVEN_OK;
        }
    }
    return last;
}

/* Find an idle connection for this URL or make one. */
/* Whether `host` is one of those `no_proxy` names: an entry matches the host itself and every
 * name under it, `*` matches everything, and nothing is known about addresses beyond their
 * spelling. */
static bool cl_no_proxy(const proven_http_client_t *c, proven_u8str_view_t host) {
    const proven_u8str_view_t list = c->cfg.no_proxy;
    /* An address is matched whole: `0.0.1` is not the end of `10.0.0.1` the way `example.org`
     * is the end of `www.example.org`. */
    proven_net_addr_t literal;
    const bool is_address = proven_net_addr_parse(host, 0, &literal) == PROVEN_OK;
    proven_size_t at = 0;
    while (at < list.size) {
        proven_size_t end = at;
        while (end < list.size && list.ptr[end] != ',') end++;
        proven_size_t from = at, to = end;
        while (from < to && (list.ptr[from] == ' ' || list.ptr[from] == '\t')) from++;
        while (to > from && (list.ptr[to - 1] == ' ' || list.ptr[to - 1] == '\t')) to--;
        at = end + 1;
        if (to - from == 1 && list.ptr[from] == '*') return true;
        if (to - from >= 2 && list.ptr[from] == '[' && list.ptr[to - 1] == ']') { from++; to--; }   /* `[::1]` is `::1` */
        else if (from < to && list.ptr[from] == '.' && !is_address) from++;                          /* `.example.org` is `example.org` */
        const proven_u8str_view_t entry = { .ptr = list.ptr + from, .size = to - from };
        if (entry.size == 0 || entry.size > host.size) continue;
        const proven_u8str_view_t tail = { .ptr = host.ptr + (host.size - entry.size), .size = entry.size };
        if (!proven_u8str_view_eq_ascii_ci(tail, entry)) continue;
        if (entry.size == host.size) return true;
        if (!is_address && host.ptr[host.size - entry.size - 1] == '.') return true;
    }
    return false;
}

static proven_err_t cl_connect(proven_http_client_t *c, const proven_url_t *u, bool allow_reuse, proven_cl_conn_t **out, bool *reused) {
    *out = (void *)0;
    *reused = false;
    bool https = proven_url_scheme_is(u, cl_lit("https"));
    proven_u16 port = proven_url_effective_port(u);

    /* The key: where the connection ends up, whatever it goes through. */
    proven_byte_t key[300];
    proven_size_t kl = 0;
    if (u->host.size > 255) return PROVEN_ERR_INVALID_FORMAT;
    const char *scheme = https ? "https://" : "http://";
    for (proven_size_t i = 0; scheme[i] != '\0'; ++i) key[kl++] = (proven_byte_t)scheme[i];
    for (proven_size_t i = 0; i < u->host.size; ++i) key[kl++] = cl_lower(u->host.ptr[i]);
    key[kl++] = ':';
    key[kl++] = (proven_byte_t)('0' + port / 10000 % 10); key[kl++] = (proven_byte_t)('0' + port / 1000 % 10);
    key[kl++] = (proven_byte_t)('0' + port / 100 % 10); key[kl++] = (proven_byte_t)('0' + port / 10 % 10);
    key[kl++] = (proven_byte_t)('0' + port % 10);

    if (allow_reuse) {
        proven_cl_conn_t *k = cl_pool_take(c, (proven_u8str_view_t){ .ptr = key, .size = kl });
        if (k) { *out = k; *reused = true; return PROVEN_OK; }
    }

    proven_cl_conn_t *conn = cl_alloc(c->cfg.alloc, sizeof *conn);
    if (!conn) return PROVEN_ERR_NOMEM;
    *conn = (proven_cl_conn_t){0};
    for (proven_size_t i = 0; i < kl; ++i) conn->key[i] = key[i];
    conn->key_len = kl;

    proven_net_deadline_t until = proven_net_deadline_in(c->cfg.connect_timeout_ms);
    proven_err_t e;
    const bool via_proxy = c->has_proxy && !cl_no_proxy(c, u->host);
    if (via_proxy) {
        proven_u16 pport = c->proxy.has_port ? c->proxy.port : (c->proxy_socks ? 1080 : 8080);
        e = cl_dial(c, c->proxy.host, pport, until, &conn->sock);
    } else {
        e = cl_dial(c, u->host, port, until, &conn->sock);
    }
    if (e != PROVEN_OK) { cl_free(c->cfg.alloc, conn); return e; }
    conn->transport = proven_net_conn_transport(&conn->sock);

    if (via_proxy) {
        if (c->proxy_socks) {
            e = cl_socks5(c, conn->transport, u->host, port, until);
        } else if (https) {
            /* "host:port", with an IPv6 literal back in its brackets. */
            proven_byte_t authority[300];
            proven_size_t al = 0;
            if (u->host_is_ipv6) authority[al++] = '[';
            for (proven_size_t i = 0; i < u->host.size; ++i) authority[al++] = u->host.ptr[i];
            if (u->host_is_ipv6) authority[al++] = ']';
            authority[al++] = ':';
            proven_byte_t digits[5];
            proven_size_t dn = 0;
            for (proven_u16 p = port; p > 0 || dn == 0; p = (proven_u16)(p / 10)) digits[dn++] = (proven_byte_t)('0' + p % 10);
            while (dn > 0) authority[al++] = digits[--dn];
            e = cl_http_tunnel(c, conn->transport, (proven_u8str_view_t){ .ptr = authority, .size = al }, until);
        } else {
            conn->via_http_proxy = true;
            e = PROVEN_OK;
        }
        if (e != PROVEN_OK) { cl_conn_close(c, conn); return e; }
    }
    if (https) {
        proven_transport_t secure = {0};
        e = c->cfg.tls_wrap(c->cfg.tls_ctx, conn->transport, u->host, until, &secure);
        if (e != PROVEN_OK || !proven_transport_is_valid(secure)) {
            cl_conn_close(c, conn);
            return e != PROVEN_OK ? e : PROVEN_ERR_INVALID_STATE;
        }
        conn->transport = secure;
        conn->wrapped = true;
    }
    *out = conn;
    return PROVEN_OK;
}

// -----------------------------------------------------------------------------
// One exchange
// -----------------------------------------------------------------------------

/* A connection that owns itself: what an upgrade hands out. */
static proven_result_size_t cl_owned_read(void *ctx, proven_mem_mut_t dest, proven_net_deadline_t until) {
    return proven_transport_read(((proven_cl_conn_t *)ctx)->transport, dest, until);
}
static proven_result_size_t cl_owned_write(void *ctx, proven_mem_view_t src, proven_net_deadline_t until) {
    return proven_transport_write(((proven_cl_conn_t *)ctx)->transport, src, until);
}
static proven_err_t cl_owned_shutdown(void *ctx) {
    return proven_transport_shutdown(((proven_cl_conn_t *)ctx)->transport);
}
static proven_err_t cl_owned_close(void *ctx) {
    proven_cl_conn_t *conn = ctx;
    proven_allocator_t a = conn->alloc;
    proven_err_t e = PROVEN_OK;
    if (conn->wrapped) e = proven_transport_close(conn->transport);
    proven_err_t e2 = proven_net_close(&conn->sock);
    cl_free(a, conn);
    return e != PROVEN_OK ? e : e2;
}

static bool cl_is_reserved_header(proven_u8str_view_t name) {
    return proven_u8str_view_eq_ascii_ci(name, cl_lit("Host")) || proven_u8str_view_eq_ascii_ci(name, cl_lit("Content-Length")) ||
           proven_u8str_view_eq_ascii_ci(name, cl_lit("Transfer-Encoding")) || proven_u8str_view_eq_ascii_ci(name, cl_lit("Connection"));
}

static bool cl_is_credential_header(proven_u8str_view_t name) {
    return proven_u8str_view_eq_ascii_ci(name, cl_lit("Authorization")) || proven_u8str_view_eq_ascii_ci(name, cl_lit("Cookie")) ||
           proven_u8str_view_eq_ascii_ci(name, cl_lit("Proxy-Authorization"));
}

/* What varies between the attempts of one send. */
typedef struct {
    proven_u8str_view_t method;
    bool send_body;
    bool cross_origin;                 /* a redirect left the original host: drop the caller's credentials */
    proven_u8str_view_t authorization; /* the answer to a challenge, when there is one */
    proven_u8str_view_t proxy_authorization; /* the answer to a proxy's Digest challenge, instead of the userinfo's Basic */
} proven_cl_attempt_t;

/* Write the request and read the response head into `call`. */
static proven_err_t cl_exchange(proven_cl_call_t *call, const proven_http_client_request_t *req, const proven_url_t *u, const proven_cl_attempt_t *at) {
    proven_http_client_t *c = call->client;
    proven_cl_conn_t *conn = call->conn;
    proven_mem_mut_t out = { .ptr = call->out, .size = call->out_cap };
    proven_size_t len = 0;
    bool https = proven_url_scheme_is(u, cl_lit("https"));
    bool streaming = at->send_body && proven_reader_is_valid(req->body_stream);
    /* The server is asked before the body is sent - only when there is a body to hold back. */
    const bool expecting = req->expect_continue && at->send_body && (streaming || req->body.size > 0);

    /* The target: the path and query, or the whole URL when an HTTP proxy is to fetch it. */
    proven_byte_t target[CL_URL_MAX + 8];
    proven_size_t tl = 0;
    if (conn->via_http_proxy) {
        proven_size_t end = (proven_size_t)(u->path.ptr + u->path.size - call->url);
        if (u->has_query) end = (proven_size_t)(u->query.ptr + u->query.size - call->url);
        for (proven_size_t i = 0; i < end; ++i) target[tl++] = call->url[i];
        if (u->path.size == 0 && !u->has_query) target[tl++] = '/';
    } else {
        if (u->path.size == 0) target[tl++] = '/';
        for (proven_size_t i = 0; i < u->path.size; ++i) target[tl++] = u->path.ptr[i];
        if (u->has_query) {
            target[tl++] = '?';
            for (proven_size_t i = 0; i < u->query.size; ++i) target[tl++] = u->query.ptr[i];
        }
    }
    proven_err_t e = proven_http_write_request_line(out, &len, at->method, (proven_u8str_view_t){ .ptr = target, .size = tl });

    /* Host: the port is written only when it is not the scheme's own. */
    proven_byte_t host[300];
    proven_size_t hl = 0;
    if (u->host_is_ipv6) host[hl++] = '[';
    for (proven_size_t i = 0; i < u->host.size; ++i) host[hl++] = u->host.ptr[i];
    if (u->host_is_ipv6) host[hl++] = ']';
    if (u->has_port && u->port != proven_url_default_port(u->scheme)) {
        host[hl++] = ':';
        char digits[5];
        int dn = 0;
        proven_u16 p = u->port;
        do { digits[dn++] = (char)('0' + p % 10); p /= 10; } while (p);
        while (dn > 0) host[hl++] = (proven_byte_t)digits[--dn];
    }
    if (e == PROVEN_OK) e = proven_http_write_header(out, &len, cl_lit("Host"), (proven_u8str_view_t){ .ptr = host, .size = hl });
    if (e == PROVEN_OK && c->cfg.user_agent.size > 0) e = proven_http_write_header(out, &len, cl_lit("User-Agent"), c->cfg.user_agent);

    bool caller_cookie = false;
    for (proven_size_t i = 0; i < req->header_count && e == PROVEN_OK; ++i) {
        proven_u8str_view_t name = req->headers[i].name;
        if (at->cross_origin && cl_is_credential_header(name)) continue;
        if (at->authorization.size > 0 && proven_u8str_view_eq_ascii_ci(name, cl_lit("Authorization"))) continue;
        if (!at->send_body && (proven_u8str_view_eq_ascii_ci(name, cl_lit("Content-Type")))) continue;
        if (expecting && proven_u8str_view_eq_ascii_ci(name, cl_lit("Expect"))) continue;      /* this call writes it */
        if (proven_u8str_view_eq_ascii_ci(name, cl_lit("Cookie"))) caller_cookie = true;
        e = proven_http_write_header(out, &len, name, req->headers[i].value);
    }
    if (e == PROVEN_OK && at->authorization.size > 0) e = proven_http_write_header(out, &len, cl_lit("Authorization"), at->authorization);
    if (e == PROVEN_OK && conn->via_http_proxy) {
        e = at->proxy_authorization.size > 0 ? proven_http_write_header(out, &len, cl_lit("Proxy-Authorization"), at->proxy_authorization)
                                             : cl_proxy_auth(c, out, &len);
    }

    if (e == PROVEN_OK && c->cfg.cookies && !caller_cookie) {
        /* The jar writes its value straight after "Cookie: "; nothing is written if it is empty. */
        static const char prefix[] = "Cookie: ";
        if (out.size - len > 10) {
            proven_size_t n = 0;
            proven_err_t ce = proven_http_cookie_jar_header(c->cfg.cookies, u->host, u->path.size ? u->path : cl_lit("/"), https, proven_time_now(),
                                                            (proven_mem_mut_t){ .ptr = out.ptr + len + 8, .size = out.size - len - 10 }, &n);
            if (ce == PROVEN_OK && n > 0) {
                for (int i = 0; i < 8; ++i) out.ptr[len + (proven_size_t)i] = (proven_byte_t)prefix[i];
                len += 8 + n;
                out.ptr[len++] = '\r';
                out.ptr[len++] = '\n';
            } else if (ce != PROVEN_OK) {
                e = ce;
            }
        } else {
            e = PROVEN_ERR_OUT_OF_BOUNDS;
        }
    }

    if (e == PROVEN_OK && c->cfg.decompress && req->upgrade.size == 0 &&
        !proven_http_header_find(req->headers, req->header_count, cl_lit("Accept-Encoding"), &(proven_u8str_view_t){0}) &&
        !proven_http_header_find(req->headers, req->header_count, cl_lit("Range"), &(proven_u8str_view_t){0})) {
        e = proven_http_write_header(out, &len, cl_lit("Accept-Encoding"), proven_http_decoder_accept_(c->cfg.decompress_zstd, c->cfg.decompress_brotli));
    }

    bool wants_body = at->send_body && (streaming || req->body.size > 0);
    bool body_method = proven_u8str_view_eq_ascii_ci(at->method, cl_lit("POST")) || proven_u8str_view_eq_ascii_ci(at->method, cl_lit("PUT")) || proven_u8str_view_eq_ascii_ci(at->method, cl_lit("PATCH"));
    if (e == PROVEN_OK) {
        if (streaming) e = proven_http_write_header(out, &len, cl_lit("Transfer-Encoding"), cl_lit("chunked"));
        else if (wants_body || body_method) e = proven_http_write_header_u64(out, &len, cl_lit("Content-Length"), at->send_body ? req->body.size : 0);
        if (e == PROVEN_OK && expecting) e = proven_http_write_header(out, &len, cl_lit("Expect"), cl_lit("100-continue"));
    }
    if (e == PROVEN_OK && req->upgrade.size > 0) {
        /* Asking to change protocol: the two headers that say so, written here because
         * Connection is the client's to write. */
        e = proven_http_write_header(out, &len, cl_lit("Connection"), cl_lit("Upgrade"));
        if (e == PROVEN_OK) e = proven_http_write_header(out, &len, cl_lit("Upgrade"), req->upgrade);
    } else if (e == PROVEN_OK && c->cfg.max_idle_connections == 0) {
        e = proven_http_write_header(out, &len, cl_lit("Connection"), cl_lit("close"));
    }
    if (e == PROVEN_OK) e = proven_http_write_head_end(out, &len);
    if (e != PROVEN_OK) return e;

    proven_transport_t t = conn->transport;
    /* Early data (RFC-0027) is for what is safe to repeat: GET, HEAD or OPTIONS with no body and
     * no change of protocol. Anything else waits for the handshake. */
    const bool safe = (proven_u8str_view_eq(at->method, cl_lit("GET")) || proven_u8str_view_eq(at->method, cl_lit("HEAD")) || proven_u8str_view_eq(at->method, cl_lit("OPTIONS"))) &&
                      !wants_body && req->upgrade.size == 0;
    if (!safe && conn->wrapped) proven_tls_transport_no_early_now_(t);
    proven_result_size_t w = proven_transport_write_all(t, (proven_mem_view_t){ .ptr = out.ptr, .size = len }, cl_io_deadline(c));
    if (w.err != PROVEN_OK) return w.err;
    call->buf_len = 0;
    call->buf_pos = 0;
    unsigned interim = 0;
    /* A body that was not sent, or not all of it: the connection is then not kept, whatever
     * the response says - the server and this side no longer agree on where the request ends. */
    bool body_unsent = false;
    /* A write that failed while the body was going out. The server may have answered before
     * it stopped listening - 413, 401 - and that answer is worth more than "the write failed":
     * the response is read, and only if there is none is this the error. (A write that timed
     * out is not such a case: the connection is still there, and a second wait on it would
     * make the call take twice its time.) */
    proven_err_t write_failed = PROVEN_OK;
    if (expecting) {
        /* The head has gone with `Expect: 100-continue`: the server's word first. 100 means go
         * on; a final answer means the body is not wanted; silence for expect_timeout_ms means
         * a server that does not know the rule, and the body goes as it would have. */
        const proven_net_deadline_t word_by = proven_net_deadline_in(c->cfg.expect_timeout_ms);
        for (;;) {
            proven_size_t head = 0;
            e = proven_http_parse_response((proven_mem_view_t){ .ptr = call->buf, .size = call->buf_len }, call->headers, CL_MAX_HEADERS,
                                           c->cfg.max_head_bytes, &call->res, &head);
            if (e == PROVEN_OK) {
                if (call->res.status >= 200 || call->res.status == 101) { body_unsent = true; break; }
                if (++interim > CL_MAX_INTERIM) return PROVEN_ERR_INVALID_FORMAT;
                const bool go_on = call->res.status == 100;
                proven_size_t rest = call->buf_len - head;
                for (proven_size_t i = 0; i < rest; ++i) call->buf[i] = call->buf[head + i];
                call->buf_len = rest;
                if (!go_on) continue;
                /* (More behind the 100 already: the server is answering, not waiting.) */
                if (rest > 0) body_unsent = true;
                break;
            }
            if (e != PROVEN_ERR_NEED_MORE) return e;
            if (call->buf_len == call->buf_cap) return PROVEN_ERR_OUT_OF_BOUNDS;
            proven_result_size_t r = proven_transport_read(t, (proven_mem_mut_t){ .ptr = call->buf + call->buf_len, .size = call->buf_cap - call->buf_len }, word_by);
            if (r.err == PROVEN_ERR_TIMEOUT || r.err == PROVEN_ERR_AGAIN) {
                if (call->buf_len > 0) body_unsent = true;       /* half an answer: it is answering */
                break;
            }
            if (r.err == PROVEN_ERR_EOF) return PROVEN_ERR_RESET;
            if (r.err != PROVEN_OK) return r.err;
            call->got_response_bytes = true;
            call->buf_len += r.value;
        }
    }
    if (body_unsent) {
        /* Nothing more is written. */
    } else if (wants_body && !streaming) {
        w = proven_transport_write_all(t, req->body, cl_io_deadline(c));
        if (w.err == PROVEN_ERR_TIMEOUT) return w.err;
        if (w.err != PROVEN_OK) { write_failed = w.err; body_unsent = true; }
    } else if (streaming) {
        /* Each piece the reader gives becomes one chunk. The head buffer is free until the
         * response arrives, so it serves as the copy buffer. */
        for (;;) {
            proven_result_size_t r = proven_reader_read(req->body_stream, (proven_mem_mut_t){ .ptr = call->buf, .size = call->buf_cap });
            if (r.err == PROVEN_ERR_EOF) break;
            if (r.err != PROVEN_OK) return r.err;
            proven_byte_t frame[24];
            proven_size_t fl = 0;
            e = proven_http_write_chunk_begin((proven_mem_mut_t){ .ptr = frame, .size = sizeof frame }, &fl, r.value);
            if (e != PROVEN_OK) return e;
            w = proven_transport_write_all(t, (proven_mem_view_t){ .ptr = frame, .size = fl }, cl_io_deadline(c));
            if (w.err == PROVEN_OK) w = proven_transport_write_all(t, (proven_mem_view_t){ .ptr = call->buf, .size = r.value }, cl_io_deadline(c));
            if (w.err == PROVEN_OK) w = proven_transport_write_all(t, (proven_mem_view_t){ .ptr = (const proven_byte_t *)"\r\n", .size = 2 }, cl_io_deadline(c));
            if (w.err != PROVEN_OK) break;
        }
        if (w.err == PROVEN_OK && req->trailer_count > 0) {
            /* The last chunk with the caller's trailers, written into the (free) head buffer. */
            proven_size_t tl = 0;
            e = proven_http_write_trailers((proven_mem_mut_t){ .ptr = call->buf, .size = call->buf_cap }, &tl, req->trailers, req->trailer_count);
            if (e != PROVEN_OK) return e;
            w = proven_transport_write_all(t, (proven_mem_view_t){ .ptr = call->buf, .size = tl }, cl_io_deadline(c));
        } else if (w.err == PROVEN_OK) {
            w = proven_transport_write_all(t, (proven_mem_view_t){ .ptr = (const proven_byte_t *)"0\r\n\r\n", .size = 5 }, cl_io_deadline(c));
        }
        if (w.err == PROVEN_ERR_TIMEOUT) return w.err;
        if (w.err != PROVEN_OK) { write_failed = w.err; body_unsent = true; }
    }

    /* The head. Interim responses (100 Continue, 103 Early Hints) are read and passed over;
     * 101 is a final answer. */
    for (;;) {
        proven_size_t head = 0;
        e = proven_http_parse_response((proven_mem_view_t){ .ptr = call->buf, .size = call->buf_len }, call->headers, CL_MAX_HEADERS,
                                       c->cfg.max_head_bytes, &call->res, &head);
        if (e == PROVEN_OK) {
            if (call->res.status >= 100 && call->res.status < 200 && call->res.status != 101) {
                /* A server may send several; one that sends them without end is holding the
                 * caller, and each would restart the read timeout. */
                if (++interim > CL_MAX_INTERIM) return PROVEN_ERR_INVALID_FORMAT;
                proven_size_t rest = call->buf_len - head;
                for (proven_size_t i = 0; i < rest; ++i) call->buf[i] = call->buf[head + i];
                call->buf_len = rest;
                continue;
            }
            call->buf_pos = head;
            break;
        }
        if (e != PROVEN_ERR_NEED_MORE) return write_failed != PROVEN_OK ? write_failed : e;
        if (call->buf_len == call->buf_cap) return PROVEN_ERR_OUT_OF_BOUNDS;
        proven_result_size_t r = proven_transport_read(t, (proven_mem_mut_t){ .ptr = call->buf + call->buf_len, .size = call->buf_cap - call->buf_len },
                                                       cl_io_deadline(c));
        /* (After a write that failed, no answer means the failure is the answer.) */
        if (r.err != PROVEN_OK && write_failed != PROVEN_OK) return write_failed;
        if (r.err == PROVEN_ERR_EOF) return PROVEN_ERR_RESET;
        if (r.err != PROVEN_OK) return r.err;
        call->got_response_bytes = true;
        call->buf_len += r.value;
    }

    if (c->cfg.cookies) {
        for (proven_size_t i = 0; i < call->res.header_count; ++i) {
            if (!proven_u8str_view_eq_ascii_ci(call->headers[i].name, cl_lit("Set-Cookie"))) continue;
            /* A cookie the jar will not take is the server's problem, not this request's. */
            (void)proven_http_cookie_jar_store(c->cfg.cookies, u->host, u->path.size ? u->path : cl_lit("/"), https,
                                               call->headers[i].value, proven_time_now());
        }
    }

    proven_http_framing_t framing;
    e = proven_http_response_framing(&call->res, proven_http_method_from_text(at->method), &framing);
    if (e != PROVEN_OK) return e;
    e = proven_http_body_init(&call->body, framing, UINT64_MAX);
    if (e == PROVEN_OK && framing.kind == PROVEN_HTTP_BODY_CHUNKED) e = proven_http_body_keep_trailers(&call->body, (proven_mem_mut_t){ .ptr = call->trailers, .size = sizeof call->trailers });
    if (e != PROVEN_OK) return e;
    call->body_done = framing.kind == PROVEN_HTTP_BODY_NONE || (framing.kind == PROVEN_HTTP_BODY_LENGTH && framing.length == 0);
    call->keep_alive = proven_http_response_keep_alive(&call->res) && framing.kind != PROVEN_HTTP_BODY_UNTIL_CLOSE &&
                       c->cfg.max_idle_connections > 0 && call->res.status != 101 && !body_unsent;
    proven_http_coding_t coding = PROVEN_HTTP_CODING_IDENTITY;
    proven_http_decoder_free_(&call->dec);
    call->pending = (proven_mem_view_t){0};
    call->decoding = c->cfg.decompress && !call->body_done &&
                     proven_http_decoder_wanted_(call->res.status, call->res.headers, call->res.header_count, c->cfg.decompress_zstd, c->cfg.decompress_brotli, &coding);
    if (call->decoding) proven_http_decoder_begin_(&call->dec, c->cfg.alloc, coding);
    return PROVEN_OK;
}

#if !defined(PROVEN_NO_HTTP3)
// -----------------------------------------------------------------------------
// HTTP/3 (RFC-0034): the event client underneath
// -----------------------------------------------------------------------------

static proven_err_t cl_h3_ready(proven_http_client_t *c) {
    if (c->ev) return PROVEN_OK;
    if (!c->loop) {
        proven_err_t e = proven_loop_create(c->cfg.alloc, &c->loop);
        if (e != PROVEN_OK) { c->loop = (void *)0; return e; }
    }
    bool offers_h2 = false;
    for (proven_size_t i = 0;; ++i) {
        const proven_u8str_view_t name = proven_tls_config_alpn(c->cfg.tls, i);
        if (name.size == 0) break;
        if (name.size == 2 && name.ptr[0] == 'h' && name.ptr[1] == '2') offers_h2 = true;
    }
    const proven_http_event_client_config_t ec = {
        .alloc = c->cfg.alloc, .tls = c->cfg.tls, .http2 = offers_h2, .http3 = true, .http3_fallback = c->cfg.http3_fallback,
        .connect_timeout_ms = c->cfg.connect_timeout_ms, .max_head_bytes = c->cfg.max_head_bytes, .max_headers = CL_MAX_HEADERS,
        .response_timeout_ms = 3600000u, .body_timeout_ms = 3600000u,        /* the waits here are bounded by io_timeout_ms; these only must not come first */
        .write_timeout_ms = c->cfg.io_timeout_ms,
        .retry_unsent = 1,                            /* a kept connection found dead before the request was on it: a new one */
    };
    return proven_http_event_client_create(c->loop, &ec, &c->ev);
}

/* Run the loop until `*flag`, or until `until`: PROVEN_ERR_TIMEOUT. */
static proven_err_t cl_h3_wait(proven_http_client_t *c, const bool *flag, const bool *also, proven_net_deadline_t until) {
    while (!*flag && !(also && *also)) {
        const proven_net_deadline_t now = proven_time_monotonic_now();
        if (now >= until) return PROVEN_ERR_TIMEOUT;
        const proven_net_deadline_t step = proven_net_deadline_in(50);
        proven_err_t e = proven_loop_poll(c->loop, step < until ? step : until);
        if (e != PROVEN_OK) return e;
    }
    return PROVEN_OK;
}

static void cl_h3_response(void *ctx, proven_http_event_request_t *r, const proven_http_response_t *head) {
    (void)r;
    proven_cl_call_t *call = ctx;
    /* The head, copied into the call where an HTTP/1.1 head would be: its views live until finish. */
    proven_size_t at = 0, n = 0;
    for (proven_size_t i = 0; i < head->header_count && n < CL_MAX_HEADERS; ++i) {
        const proven_u8str_view_t name = head->headers[i].name, value = head->headers[i].value;
        if (name.size + value.size > call->buf_cap - at) break;
        for (proven_size_t k = 0; k < name.size; ++k) call->buf[at + k] = name.ptr[k];
        for (proven_size_t k = 0; k < value.size; ++k) call->buf[at + name.size + k] = value.ptr[k];
        call->headers[n++] = (proven_http_header_t){ .name = { .ptr = call->buf + at, .size = name.size }, .value = { .ptr = call->buf + at + name.size, .size = value.size } };
        at += name.size + value.size;
    }
    call->res = (proven_http_response_t){ .version_minor = 1, .status = head->status, .headers = call->headers, .header_count = n };
    call->h3_head = true;
}

static void cl_h3_body(void *ctx, proven_http_event_request_t *r, proven_mem_view_t piece, bool last) {
    proven_cl_call_t *call = ctx;
    (void)last;
    if (piece.size == 0) return;
    if (call->h3q_len + piece.size > call->h3q_cap) {
        if (call->h3q_pos > 0) {
            for (proven_size_t i = call->h3q_pos; i < call->h3q_len; ++i) call->h3q[i - call->h3q_pos] = call->h3q[i];
            call->h3q_len -= call->h3q_pos;
            call->h3q_pos = 0;
        }
        if (call->h3q_len + piece.size > call->h3q_cap) {
            proven_size_t want = call->h3q_cap ? call->h3q_cap : 16384;
            while (want < call->h3q_len + piece.size) want *= 2;
            proven_byte_t *q = cl_alloc(call->client->cfg.alloc, want);
            if (!q) { call->h3_why = PROVEN_ERR_NOMEM; proven_http_event_request_abort(r); return; }
            for (proven_size_t i = 0; i < call->h3q_len; ++i) q[i] = call->h3q[i];
            cl_free(call->client->cfg.alloc, call->h3q);
            call->h3q = q;
            call->h3q_cap = want;
        }
    }
    for (proven_size_t i = 0; i < piece.size; ++i) call->h3q[call->h3q_len + i] = piece.ptr[i];
    call->h3q_len += piece.size;
    /* Read at the caller's pace: past the high mark the request waits (QUIC's window then holds the server). */
    if (!call->h3_paused && call->h3q_len - call->h3q_pos > CL_H3_QUEUE_HIGH) { call->h3_paused = true; proven_http_event_request_pause(r); }
}

/* A stream body: what the reader gives, written as the request takes it. */
static void cl_h3_pump(proven_cl_call_t *call) {
    while (call->h3_req && !call->h3_done && !call->h3_stream_end) {
        if (call->h3_pend_off == call->h3_pend_len) {
            proven_result_size_t r = proven_reader_read(call->h3_stream, (proven_mem_mut_t){ .ptr = call->out, .size = call->out_cap });
            if (r.err == PROVEN_ERR_EOF) {
                call->h3_stream_end = true;
                (void)proven_http_event_request_end(call->h3_req);
                return;
            }
            if (r.err != PROVEN_OK) { call->h3_why = r.err; proven_http_event_request_abort(call->h3_req); return; }
            call->h3_pend_off = 0;
            call->h3_pend_len = r.value;
        }
        proven_result_size_t w = proven_http_event_request_write(call->h3_req, (proven_mem_view_t){ .ptr = call->out + call->h3_pend_off, .size = call->h3_pend_len - call->h3_pend_off });
        if (w.err != PROVEN_OK || w.value == 0) return;          /* on_writable says when */
        call->h3_pend_off += w.value;
    }
}

static void cl_h3_writable(void *ctx, proven_http_event_request_t *r) { (void)r; cl_h3_pump(ctx); }

static void cl_h3_done(void *ctx, proven_http_event_request_t *r, proven_err_t why) {
    (void)r;
    proven_cl_call_t *call = ctx;
    call->h3_done = true;
    if (call->h3_why == PROVEN_OK) call->h3_why = why;
    call->h3_req = (void *)0;
}

/* The request over HTTP/3, and its response head into `call`. */
static proven_err_t cl_exchange_h3(proven_cl_call_t *call, const proven_http_client_request_t *req, const proven_url_t *u, const proven_cl_attempt_t *at) {
    proven_http_client_t *c = call->client;
    proven_err_t e = cl_h3_ready(c);
    if (e != PROVEN_OK) return e;
    call->h3 = true;
    call->h3_head = call->h3_done = call->h3_paused = call->h3_stream_end = false;
    call->h3_why = PROVEN_OK;
    call->h3q_len = call->h3q_pos = 0;
    call->h3_pend_off = call->h3_pend_len = 0;

    /* The fields, chosen as the HTTP/1.1 exchange chooses them; their bytes in `out`. */
    proven_http_header_t fields[CL_MAX_HEADERS + 8];
    proven_size_t nf = 0, used = 0;
    bool caller_cookie = false;
    const bool streaming = at->send_body && proven_reader_is_valid(req->body_stream);
    for (proven_size_t i = 0; i < req->header_count; ++i) {
        const proven_u8str_view_t name = req->headers[i].name;
        if (at->cross_origin && cl_is_credential_header(name)) continue;
        if (at->authorization.size > 0 && proven_u8str_view_eq_ascii_ci(name, cl_lit("Authorization"))) continue;
        if (!at->send_body && proven_u8str_view_eq_ascii_ci(name, cl_lit("Content-Type"))) continue;
        if (proven_u8str_view_eq_ascii_ci(name, cl_lit("Expect"))) continue;
        if (proven_u8str_view_eq_ascii_ci(name, cl_lit("Cookie"))) caller_cookie = true;
        if (nf == CL_MAX_HEADERS) return PROVEN_ERR_OUT_OF_BOUNDS;
        fields[nf++] = req->headers[i];
    }
    if (c->cfg.user_agent.size > 0) fields[nf++] = (proven_http_header_t){ cl_lit("User-Agent"), c->cfg.user_agent };
    if (at->authorization.size > 0) fields[nf++] = (proven_http_header_t){ cl_lit("Authorization"), at->authorization };
    if (c->cfg.cookies && !caller_cookie) {
        proven_size_t n = 0;
        e = proven_http_cookie_jar_header(c->cfg.cookies, u->host, u->path.size ? u->path : cl_lit("/"), true, proven_time_now(),
                                          (proven_mem_mut_t){ .ptr = call->out + used, .size = call->out_cap - used }, &n);
        if (e != PROVEN_OK) return e;
        if (n > 0) { fields[nf++] = (proven_http_header_t){ cl_lit("Cookie"), { .ptr = call->out + used, .size = n } }; used += n; }
    }
    if (c->cfg.decompress && !proven_http_header_find(req->headers, req->header_count, cl_lit("Accept-Encoding"), &(proven_u8str_view_t){0}) &&
        !proven_http_header_find(req->headers, req->header_count, cl_lit("Range"), &(proven_u8str_view_t){0}))
        fields[nf++] = (proven_http_header_t){ cl_lit("Accept-Encoding"), proven_http_decoder_accept_(c->cfg.decompress_zstd, c->cfg.decompress_brotli) };

    /* Where: the event client takes an address. */
    proven_net_addr_t addrs[CL_MAX_ADDRS];
    proven_size_t count = 0;
    e = proven_net_resolve(u->host, proven_url_effective_port(u), addrs, CL_MAX_ADDRS, &count);
    if (e != PROVEN_OK) return e;
    if (count == 0) return PROVEN_ERR_NOT_FOUND;

    /* Each address in turn while one gives no QUIC answer at all, as the TCP connect tries each. */
    for (proven_size_t ai = 0;; ++ai) {
        proven_http_event_request_options_t o = {
            .method = at->method, .url = { .ptr = call->url, .size = call->url_len }, .address = &addrs[ai], .headers = fields, .header_count = nf,
            .on = { cl_h3_response, cl_h3_body, cl_h3_writable, cl_h3_done }, .ctx = call, .http3 = true,
        };
        if (at->send_body && !streaming) o.body = req->body;
        if (streaming) { o.body_length = PROVEN_HTTP_EVENT_LENGTH_UNKNOWN; call->h3_stream = req->body_stream; }
        else if (at->send_body && req->body.size == 0 && (proven_u8str_view_eq_ascii_ci(at->method, cl_lit("POST")) || proven_u8str_view_eq_ascii_ci(at->method, cl_lit("PUT")) ||
                                                          proven_u8str_view_eq_ascii_ci(at->method, cl_lit("PATCH")))) o.body_length = 0;
        call->h3_head = call->h3_done = false;
        call->h3_why = PROVEN_OK;
        e = proven_http_event_client_start(c->ev, &o, &call->h3_req);
        if (e != PROVEN_OK) { call->h3_req = (void *)0; return e; }
        if (streaming) cl_h3_pump(call);
        /* The head: connect_timeout_ms for the connection the event client may make, then io_timeout_ms. */
        e = cl_h3_wait(c, &call->h3_head, &call->h3_done, proven_net_deadline_in(c->cfg.connect_timeout_ms + c->cfg.io_timeout_ms));
        if (e == PROVEN_OK && !call->h3_head) e = call->h3_why != PROVEN_OK ? call->h3_why : PROVEN_ERR_RESET;
        if (e == PROVEN_OK) break;
        if (call->h3_req) proven_http_event_request_abort(call->h3_req);
        const bool nobody = e == PROVEN_ERR_TIMEOUT || e == PROVEN_ERR_REFUSED || e == PROVEN_ERR_UNREACHABLE;
        if (!nobody || streaming || ai + 1 >= count) return e;          /* a stream body was read once: not again */
    }
    call->got_response_bytes = true;
    if (c->cfg.cookies) {
        for (proven_size_t i = 0; i < call->res.header_count; ++i) {
            if (!proven_u8str_view_eq_ascii_ci(call->headers[i].name, cl_lit("Set-Cookie"))) continue;
            (void)proven_http_cookie_jar_store(c->cfg.cookies, u->host, u->path.size ? u->path : cl_lit("/"), true, call->headers[i].value, proven_time_now());
        }
    }
    call->keep_alive = false;                          /* the QUIC connection is the event client's to keep */
    call->body_done = false;
    proven_http_coding_t coding = PROVEN_HTTP_CODING_IDENTITY;
    proven_http_decoder_free_(&call->dec);
    call->pending = (proven_mem_view_t){0};
    call->decoding = c->cfg.decompress && proven_http_decoder_wanted_(call->res.status, call->res.headers, call->res.header_count, c->cfg.decompress_zstd, c->cfg.decompress_brotli, &coding);
    if (call->decoding) proven_http_decoder_begin_(&call->dec, c->cfg.alloc, coding);
    return PROVEN_OK;
}

/* Body bytes from the queue, the loop run for more while it is empty. */
static proven_err_t cl_h3_next(proven_cl_call_t *call, proven_size_t max, proven_mem_view_t *payload) {
    proven_http_client_t *c = call->client;
    if (call->h3q_pos == call->h3q_len) {
        call->h3q_pos = call->h3q_len = 0;
        if (call->h3_paused && call->h3_req) { call->h3_paused = false; proven_http_event_request_resume(call->h3_req); }
        while (call->h3q_len == 0 && !call->h3_done) {
            proven_err_t e = cl_h3_wait(c, &call->h3_done, (void *)0, proven_net_deadline_in(c->cfg.io_timeout_ms));
            if (call->h3q_len > 0) break;
            if (e != PROVEN_OK) return e;
        }
        if (call->h3q_len == 0) {
            call->body_done = true;
            return call->h3_why == PROVEN_OK ? PROVEN_ERR_EOF : (call->h3_why == PROVEN_ERR_EOF ? PROVEN_ERR_RESET : call->h3_why);
        }
    }
    proven_size_t n = call->h3q_len - call->h3q_pos;
    if (n > max) n = max;
    *payload = (proven_mem_view_t){ .ptr = call->h3q + call->h3q_pos, .size = n };
    call->h3q_pos += n;
    if (call->h3_paused && call->h3_req && call->h3q_len - call->h3q_pos < CL_H3_QUEUE_LOW) { call->h3_paused = false; proven_http_event_request_resume(call->h3_req); }
    return PROVEN_OK;
}

static void cl_h3_release(proven_cl_call_t *call) {
    if (!call->h3) return;
    if (call->h3_req) proven_http_event_request_abort(call->h3_req);
    call->h3_req = (void *)0;
    cl_free(call->client->cfg.alloc, call->h3q);
    call->h3q = (void *)0;
    call->h3q_cap = call->h3q_len = call->h3q_pos = 0;
    call->h3 = false;
}
#endif

/* The next piece of the body, as a view into the read window of at most `max` bytes. The
 * window is refilled from the transport when it runs dry - so a view is good only until the
 * next call. PROVEN_ERR_EOF at the end of the body. */
static proven_err_t cl_next_payload(proven_cl_call_t *call, proven_size_t max, proven_mem_view_t *payload) {
    if (call->body_done) return PROVEN_ERR_EOF;
#if !defined(PROVEN_NO_HTTP3)
    if (call->h3) return cl_h3_next(call, max, payload);
#endif
    for (;;) {
        if (call->buf_pos < call->buf_len) {
            proven_size_t avail = call->buf_len - call->buf_pos;
            proven_size_t offer = avail < max ? avail : max;
            proven_size_t used = 0;
            bool done = false;
            proven_err_t e = proven_http_body_feed(&call->body, (proven_mem_view_t){ .ptr = call->buf + call->buf_pos, .size = offer }, &used, payload, &done);
            if (e != PROVEN_OK) return e;
            call->buf_pos += used;
            if (done) call->body_done = true;
            if (payload->size > 0) return PROVEN_OK;
            if (done) return PROVEN_ERR_EOF;
            continue;
        }
        call->buf_pos = 0;
        call->buf_len = 0;
        proven_result_size_t r = proven_transport_read(call->conn->transport, (proven_mem_mut_t){ .ptr = call->buf, .size = call->buf_cap },
                                                       cl_io_deadline(call->client));
        if (r.err == PROVEN_ERR_EOF) {
            /* The peer closed. For a body that runs until close that is its end; for any other
             * it is a body cut short, which must not pass for a complete one. */
            call->keep_alive = false;
            if (proven_http_body_end(&call->body) == PROVEN_OK) { call->body_done = true; return PROVEN_ERR_EOF; }
            return PROVEN_ERR_RESET;
        }
        if (r.err != PROVEN_OK) return r.err;
        call->buf_len = r.value;
    }
}

/* Body bytes into `dest`, as they came. The decoder of the framing is fed no more than `dest`
 * can take, so a payload always fits. */
static proven_result_size_t cl_read_body(proven_cl_call_t *call, proven_mem_mut_t dest) {
    proven_result_size_t res = { .err = PROVEN_OK, .value = 0 };
    if (call->body_done) { res.err = PROVEN_ERR_EOF; return res; }
    if (dest.size == 0) return res;
    proven_mem_view_t payload = {0};
    res.err = cl_next_payload(call, dest.size, &payload);
    if (res.err != PROVEN_OK) return res;
    for (proven_size_t i = 0; i < payload.size; ++i) dest.ptr[i] = payload.ptr[i];
    res.value = payload.size;
    return res;
}

/* Body bytes into `dest`, decoded. The end is the end of the HTTP body, not of the compressed
 * stream inside it: the connection is reusable exactly when it would have been. */
static proven_result_size_t cl_read_decoded(proven_cl_call_t *call, proven_mem_mut_t dest) {
    proven_result_size_t res = { .err = PROVEN_OK, .value = 0 };
    if (dest.size == 0) return res;
    for (;;) {
        /* Also with nothing pending: what the decoder still holds comes out first. */
        proven_size_t used = 0, made = 0;
        res.err = proven_http_decoder_step_(&call->dec, call->pending, &used, dest, &made);
        if (res.err != PROVEN_OK) break;
        call->pending.ptr += used;
        call->pending.size -= used;
        if (made > 0) { res.value = made; return res; }
        if (call->pending.size > 0) {
            if (used > 0) continue;
            res.err = PROVEN_ERR_INVALID_FORMAT;       /* input it will not take, and nothing to give */
            break;
        }
        res.err = cl_next_payload(call, call->buf_cap, &call->pending);
        if (res.err == PROVEN_ERR_EOF) {
            res.err = proven_http_decoder_end_(&call->dec);
            if (res.err == PROVEN_OK) { res.err = PROVEN_ERR_EOF; return res; }
        }
        if (res.err != PROVEN_OK) break;
    }
    if (res.err != PROVEN_ERR_TIMEOUT) call->keep_alive = false;
    return res;
}

/* Read away what is left of a body so the connection can carry the next request - up to a
 * limit: past it, closing is cheaper than reading. */
static void cl_drain(proven_cl_call_t *call) {
#if !defined(PROVEN_NO_HTTP3)
    if (call->h3) return;                              /* the stream is reset instead: nothing is kept that a body must be read off */
#endif
    proven_byte_t sink[2048];
    proven_size_t total = 0;
    while (!call->body_done && total <= CL_DRAIN_LIMIT) {
        proven_result_size_t r = cl_read_body(call, (proven_mem_mut_t){ .ptr = sink, .size = sizeof sink });
        if (r.err != PROVEN_OK) break;
        total += r.value;
    }
    if (!call->body_done) call->keep_alive = false;
}

/* Give the connection back, or close it. */
static void cl_release_conn(proven_cl_call_t *call) {
#if !defined(PROVEN_NO_HTTP3)
    cl_h3_release(call);
#endif
    if (!call->conn) return;
    if (call->body_done && call->keep_alive && call->buf_pos == call->buf_len) cl_pool_put(call->client, call->conn);
    else cl_conn_close(call->client, call->conn);
    call->conn = (void *)0;
}

static void cl_random_hex(proven_byte_t out[32]) {
    static const char digits[] = "0123456789abcdef";
    proven_byte_t raw[16] = {0};
    if (!proven_random_bytes(raw, sizeof raw)) {
        /* No entropy source: a cnonce only has to differ between requests, which the clock
         * provides; it is not a secret. */
        proven_u64 t = (proven_u64)proven_time_monotonic_now();
        for (int i = 0; i < 8; ++i) { raw[i] = (proven_byte_t)(t >> (8 * i)); raw[8 + i] = (proven_byte_t)(~t >> (8 * i)); }
    }
    for (int i = 0; i < 16; ++i) { out[i * 2] = (proven_byte_t)digits[raw[i] >> 4]; out[i * 2 + 1] = (proven_byte_t)digits[raw[i] & 0xf]; }
}

proven_err_t proven_http_client_send(proven_http_client_t *client, const proven_http_client_request_t *request,
                                     proven_http_client_response_t *response) {
    if (response) *response = (proven_http_client_response_t){0};
    if (!client || !request || !response) return PROVEN_ERR_INVALID_ARG;
    if (request->header_count > 0 && !request->headers) return PROVEN_ERR_INVALID_ARG;
    if (request->url.size == 0 || request->url.size > CL_URL_MAX) return PROVEN_ERR_INVALID_FORMAT;
    for (proven_size_t i = 0; i < request->header_count; ++i) {
        if (cl_is_reserved_header(request->headers[i].name)) return PROVEN_ERR_INVALID_ARG;
    }
    bool streaming = proven_reader_is_valid(request->body_stream);
    if (streaming && request->body.size > 0) return PROVEN_ERR_INVALID_ARG;
    /* Trailers follow the last chunk: only a body that goes chunked has one. */
    if (request->trailer_count > 0 && (!request->trailers || !streaming)) return PROVEN_ERR_INVALID_ARG;
    for (proven_size_t i = 0; i < request->trailer_count; ++i) if (!proven_http_trailer_allowed(request->trailers[i].name)) return PROVEN_ERR_INVALID_ARG;
    if (request->trailer_count > 0 && request->http3) return PROVEN_ERR_UNSUPPORTED;
    if (request->http3 && !client->cfg.http3) return PROVEN_ERR_INVALID_ARG;
    if (request->http3 && request->upgrade.size > 0) return PROVEN_ERR_UNSUPPORTED;     /* HTTP/3 changes protocol no such way */

    proven_allocator_t a = client->cfg.alloc;
    proven_size_t cap = client->cfg.max_head_bytes;
    if (cap < 1024) cap = 1024;
    if (cap > PROVEN_SIZE_MAX / 4) return PROVEN_ERR_OVERFLOW;
    proven_cl_call_t *call = cl_alloc(a, sizeof *call + 2 * cap);
    if (!call) return PROVEN_ERR_NOMEM;
    *call = (proven_cl_call_t){0};
    call->client = client;
    call->buf = (proven_byte_t *)(call + 1);
    call->buf_cap = cap;
    call->out = call->buf + cap;
    call->out_cap = cap;
    for (proven_size_t i = 0; i < request->url.size; ++i) call->url[i] = request->url.ptr[i];
    call->url_len = request->url.size;

    proven_cl_attempt_t at = { .method = request->method.size ? request->method : cl_lit("GET"), .send_body = true, .cross_origin = false };
    proven_byte_t auth_value[1024];
    proven_byte_t proxy_value[1024];
    proven_byte_t origin_host[256];
    proven_size_t origin_host_len = 0;
    proven_u16 origin_port = 0;
    bool origin_https = false;
    bool auth_tried = false, stale_retried = false, too_early_retried = false, proxy_tried = false;
    proven_u32 redirects = 0;
    proven_err_t e = PROVEN_OK;

    for (;;) {
        proven_url_t u;
        e = proven_url_parse((proven_u8str_view_t){ .ptr = call->url, .size = call->url_len }, &u);
        if (e != PROVEN_OK || u.host.size == 0 || u.host.size > 255) { e = PROVEN_ERR_INVALID_FORMAT; break; }
        bool https = proven_url_scheme_is(&u, cl_lit("https"));
        if (!https && !proven_url_scheme_is(&u, cl_lit("http"))) { e = PROVEN_ERR_UNSUPPORTED; break; }
        if (redirects == 0 && origin_host_len == 0) {
            if (request->http3 && !https) { e = PROVEN_ERR_INVALID_ARG; break; }
            for (proven_size_t i = 0; i < u.host.size; ++i) origin_host[i] = cl_lower(u.host.ptr[i]);
            origin_host_len = u.host.size;
            origin_port = proven_url_effective_port(&u);
            origin_https = https;
        }
        /* HTTP/3 where the caller asked for it: the origin it named, and a redirect within it. */
        bool use_h3 = false;
#if !defined(PROVEN_NO_HTTP3)
        if (request->http3 && https && u.host.size == origin_host_len && proven_url_effective_port(&u) == origin_port && origin_https) {
            use_h3 = true;
            for (proven_size_t i = 0; use_h3 && i < u.host.size; ++i) if (cl_lower(u.host.ptr[i]) != origin_host[i]) use_h3 = false;
        }
        if (use_h3 && client->has_proxy && !cl_no_proxy(client, u.host)) { e = PROVEN_ERR_UNSUPPORTED; break; }   /* no CONNECT-UDP */
#endif
        if (https && !use_h3 && !client->cfg.tls_wrap) { e = PROVEN_ERR_UNSUPPORTED; break; }

        /* A request is replayed on a fresh connection when a reused one turns out to be dead
         * before a single byte of the answer arrived - the server closed it while it sat
         * idle - and the body can be sent again. */
        bool replayable = !(at.send_body && streaming);
        bool allow_reuse = true;
#if !defined(PROVEN_NO_HTTP3)
        if (use_h3) {
            call->got_response_bytes = false;
            e = cl_exchange_h3(call, request, &u, &at);
            if (e != PROVEN_OK) break;
        }
#endif
        for (int attempt = 0; attempt < 2 && !use_h3; ++attempt) {
            e = cl_connect(client, &u, allow_reuse, &call->conn, &call->reused);
            if (e != PROVEN_OK) break;
            call->got_response_bytes = false;
            e = cl_exchange(call, request, &u, &at);
            if (e == PROVEN_OK) break;
            bool dead_idle = call->reused && !call->got_response_bytes && replayable &&
                             (e == PROVEN_ERR_RESET || e == PROVEN_ERR_EOF || e == PROVEN_ERR_TIMEOUT);
            cl_conn_close(client, call->conn);
            call->conn = (void *)0;
            if (!dead_idle) break;
            allow_reuse = false;
        }
        if (e != PROVEN_OK) break;

        proven_u16 status = call->res.status;

        /* An authentication challenge, answered once. */
        /* Credentials are for the host they were configured for: after a redirect to another
         * origin a challenge is not answered. */
        if (status == 401 && client->cfg.username.size > 0 && replayable && !at.cross_origin && (!auth_tried || !stale_retried)) {
            proven_u8str_view_t challenge;
            proven_size_t n = 0;
            bool answered = false;
            for (proven_size_t i = 0; i < call->res.header_count && !answered; ++i) {
                if (!proven_u8str_view_eq_ascii_ci(call->headers[i].name, cl_lit("WWW-Authenticate"))) continue;
                challenge = call->headers[i].value;
                proven_http_digest_challenge_t dc;
                if (proven_http_digest_challenge_parse(challenge, &dc) == PROVEN_OK) {
                    if (auth_tried && !dc.stale) continue;        /* the password was wrong: do not ask again */
                    if (auth_tried) stale_retried = true;
                    proven_byte_t cnonce[32];
                    cl_random_hex(cnonce);
                    /* The uri in the answer is the target as it was sent. */
                    proven_byte_t uri[CL_URL_MAX + 8];
                    proven_size_t ul = 0;
                    if (u.path.size == 0) uri[ul++] = '/';
                    for (proven_size_t k = 0; k < u.path.size; ++k) uri[ul++] = u.path.ptr[k];
                    if (u.has_query) { uri[ul++] = '?'; for (proven_size_t k = 0; k < u.query.size; ++k) uri[ul++] = u.query.ptr[k]; }
                    /* With auth-int the answer covers the body, which is in memory: replayable. */
                    const proven_mem_view_t body = at.send_body ? request->body : (proven_mem_view_t){0};
                    if (proven_http_digest_auth_body(&dc, client->cfg.username, client->cfg.password, at.method,
                                                     (proven_u8str_view_t){ .ptr = uri, .size = ul }, 1,
                                                     (proven_u8str_view_t){ .ptr = cnonce, .size = sizeof cnonce }, body,
                                                     (proven_mem_mut_t){ .ptr = auth_value, .size = sizeof auth_value }, &n) == PROVEN_OK) answered = true;
                }
            }
            for (proven_size_t i = 0; i < call->res.header_count && !answered && !auth_tried; ++i) {
                if (!proven_u8str_view_eq_ascii_ci(call->headers[i].name, cl_lit("WWW-Authenticate"))) continue;
                if (!proven_http_auth_offers(call->headers[i].value, cl_lit("Basic"))) continue;
                if (proven_http_basic_auth(client->cfg.username, client->cfg.password,
                                           (proven_mem_mut_t){ .ptr = auth_value, .size = sizeof auth_value }, &n) == PROVEN_OK) answered = true;
            }
            if (answered) {
                auth_tried = true;
                at.authorization = (proven_u8str_view_t){ .ptr = auth_value, .size = n };
                cl_drain(call);
                cl_release_conn(call);
                continue;
            }
        }

        /* A proxy's Digest challenge, answered once: the request goes again with the answer in
         * Proxy-Authorization (the uri is the absolute form, as the request line has it). */
        if (status == 407 && !proxy_tried && replayable && call->conn && call->conn->via_http_proxy) {
            proven_size_t end = (proven_size_t)(u.path.ptr + u.path.size - call->url);
            if (u.has_query) end = (proven_size_t)(u.query.ptr + u.query.size - call->url);
            proven_byte_t target[CL_URL_MAX + 8];
            proven_size_t tl = 0;
            for (proven_size_t i = 0; i < end; ++i) target[tl++] = call->url[i];
            if (u.path.size == 0 && !u.has_query) target[tl++] = '/';
            proven_size_t n = 0;
            if (cl_proxy_digest(client, call->headers, call->res.header_count, at.method, (proven_u8str_view_t){ .ptr = target, .size = tl },
                                proxy_value, sizeof proxy_value, &n)) {
                proxy_tried = true;
                at.proxy_authorization = (proven_u8str_view_t){ .ptr = proxy_value, .size = n };
                cl_drain(call);
                cl_release_conn(call);
                continue;
            }
        }

        /* Too Early (RFC 8470): the request went as TLS early data and the server will not act
         * on a copy that may be replayed. Once more, on a new connection, not as early data. */
        if (status == 425 && !too_early_retried && replayable && call->conn && call->conn->wrapped &&
            proven_tls_transport_early_(call->conn->transport) == PROVEN_TLS_EARLY_ACCEPTED) {
            too_early_retried = true;
            proven_tls_transport_no_early_next_(call->conn->transport);
            cl_drain(call);
            cl_conn_close(client, call->conn);
            call->conn = (void *)0;
            continue;
        }

        /* A redirect. */
        bool is_redirect = status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
        proven_u8str_view_t location;
        if (is_redirect && redirects < client->cfg.max_redirects &&
            proven_http_header_find(call->headers, call->res.header_count, cl_lit("Location"), &location)) {
            bool to_get = status == 303 || ((status == 301 || status == 302) && proven_u8str_view_eq_ascii_ci(at.method, cl_lit("POST")));
            bool must_repeat_body = !to_get && at.send_body && streaming;
            proven_byte_t next[CL_URL_MAX];
            proven_size_t nl = 0;
            proven_url_t nu;
            bool ok = !must_repeat_body &&
                      proven_url_resolve((proven_u8str_view_t){ .ptr = call->url, .size = call->url_len }, location,
                                         (proven_mem_mut_t){ .ptr = next, .size = sizeof next }, &nl) == PROVEN_OK &&
                      proven_url_parse((proven_u8str_view_t){ .ptr = next, .size = nl }, &nu) == PROVEN_OK && nu.host.size > 0;
            bool next_https = ok && proven_url_scheme_is(&nu, cl_lit("https"));
            if (ok && !next_https && !proven_url_scheme_is(&nu, cl_lit("http"))) ok = false;
            if (ok && https && !next_https) ok = false;               /* never from https down to http */
            bool same = ok && nu.host.size == origin_host_len && proven_url_effective_port(&nu) == origin_port && next_https == origin_https;
            for (proven_size_t i = 0; same && i < nu.host.size; ++i) if (cl_lower(nu.host.ptr[i]) != origin_host[i]) same = false;
            /* Within the origin a request asked over HTTP/3 stays on it; anywhere else HTTPS is tls_wrap's. */
            if (ok && next_https && !client->cfg.tls_wrap && !(request->http3 && same)) ok = false;
            if (ok) {
                /* A fragment is the client's own business and is not sent anywhere. */
                if (nu.has_fragment) nl = (proven_size_t)(nu.fragment.ptr - 1 - next);
                if (!same) at.cross_origin = true;
                /* An answer to a challenge was computed for the old target; the new one may
                 * ask again. */
                at.authorization = (proven_u8str_view_t){0};
                auth_tried = false;
                stale_retried = false;
                if (to_get) { at.method = cl_lit("GET"); at.send_body = false; }
                cl_drain(call);
                cl_release_conn(call);
                for (proven_size_t i = 0; i < nl; ++i) call->url[i] = next[i];
                call->url_len = nl;
                redirects++;
                continue;
            }
        }
        break;
    }

    if (e != PROVEN_OK) {
#if !defined(PROVEN_NO_HTTP3)
        cl_h3_release(call);
#endif
        if (call->conn) cl_conn_close(client, call->conn);
        proven_http_decoder_free_(&call->dec);
        cl_free(a, call);
        return e;
    }
    response->status = call->res.status;
    response->reason = call->res.reason;
    response->headers = call->headers;
    response->header_count = call->res.header_count;
    response->url = (proven_u8str_view_t){ .ptr = call->url, .size = call->url_len };
    response->redirects = redirects;
    response->internal = call;
    return PROVEN_OK;
}

proven_err_t proven_http_client_get(proven_http_client_t *client, proven_u8str_view_t url,
                                    proven_http_client_response_t *response) {
    proven_http_client_request_t req = {0};
    req.url = url;
    return proven_http_client_send(client, &req, response);
}

proven_result_size_t proven_http_client_read(proven_http_client_response_t *response, proven_mem_mut_t dest) {
    proven_result_size_t res = { .err = PROVEN_ERR_INVALID_ARG, .value = 0 };
    if (!response || (dest.size > 0 && !dest.ptr)) return res;
    if (!response->internal) { res.err = PROVEN_ERR_INVALID_STATE; return res; }
    proven_cl_call_t *call = response->internal;
    return call->decoding ? cl_read_decoded(call, dest) : cl_read_body(call, dest);
}

proven_err_t proven_http_client_upgrade(proven_http_client_response_t *response, proven_transport_t *out, proven_mem_view_t *early) {
    if (out) *out = (proven_transport_t){0};
    if (early) *early = (proven_mem_view_t){0};
    if (!response || !out || !early) return PROVEN_ERR_INVALID_ARG;
    proven_cl_call_t *call = response->internal;
    if (!call || !call->conn || call->res.status != 101) return PROVEN_ERR_INVALID_STATE;
    proven_cl_conn_t *conn = call->conn;
    conn->alloc = call->client->cfg.alloc;
    call->conn = (void *)0;                     /* finish no longer has a connection to close */
    out->ctx = conn;
    out->read_fn = cl_owned_read;
    out->write_fn = cl_owned_write;
    out->shutdown_fn = cl_owned_shutdown;
    out->close_fn = cl_owned_close;
    *early = (proven_mem_view_t){ .ptr = call->buf + call->buf_pos, .size = call->buf_len - call->buf_pos };
    call->buf_pos = call->buf_len;
    return PROVEN_OK;
}

proven_err_t proven_http_client_read_all(proven_http_client_response_t *response, proven_allocator_t alloc,
                                         proven_u8str_t *out, proven_size_t max_bytes) {
    if (!response || !out) return PROVEN_ERR_INVALID_ARG;
    if (!response->internal) return PROVEN_ERR_INVALID_STATE;
    proven_byte_t chunk[4096];
    proven_size_t total = 0;
    for (;;) {
        proven_size_t room = max_bytes - total;
        /* One byte past the limit is asked for, so that a body of exactly the limit ends in
         * EOF and one that is longer is seen to be longer. */
        proven_size_t want = room < sizeof chunk ? room + 1 : sizeof chunk;
        if (want > sizeof chunk) want = sizeof chunk;
        proven_result_size_t r = proven_http_client_read(response, (proven_mem_mut_t){ .ptr = chunk, .size = want });
        if (r.err == PROVEN_ERR_EOF) return PROVEN_OK;
        if (r.err != PROVEN_OK) return r.err;
        proven_size_t take = r.value > room ? room : r.value;
        proven_err_t e = proven_u8str_append_grow(alloc, out, (proven_u8str_view_t){ .ptr = chunk, .size = take });
        if (e != PROVEN_OK) return e;
        total += take;
        if (r.value > room) return PROVEN_ERR_OUT_OF_BOUNDS;
    }
}

proven_err_t proven_http_client_trailers(const proven_http_client_response_t *response, proven_http_header_t *fields, proven_size_t cap, proven_size_t *count) {
    if (count) *count = 0;
    if (!response || !count || (cap > 0 && !fields)) return PROVEN_ERR_INVALID_ARG;
    const proven_cl_call_t *call = response->internal;
    if (!call) return PROVEN_ERR_INVALID_STATE;
#if !defined(PROVEN_NO_HTTP3)
    if (call->h3) return PROVEN_ERR_UNSUPPORTED;
#endif
    if (!call->body_done) return PROVEN_ERR_INVALID_STATE;
    return proven_http_body_trailers(&call->body, fields, cap, count);
}

void proven_http_client_finish(proven_http_client_response_t *response) {
    if (!response) return;
    proven_cl_call_t *call = response->internal;
    if (call) {
        proven_allocator_t a = call->client->cfg.alloc;
        cl_release_conn(call);
        proven_http_decoder_free_(&call->dec);
        cl_free(a, call);
    }
    *response = (proven_http_client_response_t){0};
}

#else
/* A translation unit must not be empty. */
typedef int proven_http_client_unused_t;
#endif
