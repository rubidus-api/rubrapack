// src/net/http.c - HTTP/1.1 POST for timestamp servers (include/rubrapack/net.h; RFC 9110, RFC 9112).

#include "rubrapack/mem.h"
#include "rubrapack/net.h"
#include "rubrapack/pal.h"
#include "rubrapack/version.h"

#include <stdio.h>
#include <string.h>

enum { MAX_HEAD = 65536, DEFAULT_BODY = 4 << 20, CONNECT_MS = 10000, TOTAL_MS = 60000, MAX_REDIRECTS = 3, RETRIES = 2 };

static bool host_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == ':';
}

bool rp_url_parse(const char *url, bool *https, char *host, size_t host_cap, uint16_t *port, char *path, size_t path_cap) {
    const char *p;
    if (strncmp(url, "http://", 7) == 0) {
        *https = false;
        p = url + 7;
        *port = 80;
    } else if (strncmp(url, "https://", 8) == 0) {
        *https = true;
        p = url + 8;
        *port = 443;
    } else {
        return false;
    }
    const char *end = p + strcspn(p, "/?#");
    if (memchr(p, '@', (size_t)(end - p))) return false;        // no user information in a URL
    const char *h = p, *he;
    const char *portp = NULL;
    if (*p == '[') {                                            // IPv6 literal
        h = p + 1;
        he = memchr(h, ']', (size_t)(end - h));
        if (he == NULL) return false;
        if (he + 1 < end) {
            if (he[1] != ':') return false;
            portp = he + 2;
        }
    } else {
        he = memchr(p, ':', (size_t)(end - p));
        if (he) portp = he + 1;
        else he = end;
    }
    size_t hl = (size_t)(he - h);
    if (hl == 0 || hl >= host_cap) return false;
    for (size_t i = 0; i < hl; ++i) {
        if (!host_char(h[i])) return false;
    }
    memcpy(host, h, hl);
    host[hl] = 0;
    if (portp) {
        unsigned v = 0;
        if (portp >= end) return false;
        for (const char *q = portp; q < end; ++q) {
            if (*q < '0' || *q > '9' || (v = v * 10 + (unsigned)(*q - '0')) > 65535) return false;
        }
        if (v == 0) return false;
        *port = (uint16_t)v;
    }
    const char *rest = end;
    if (*rest == '#') rest = "";
    size_t rl = strcspn(rest, "#");
    if (rl + 2 > path_cap) return false;
    if (rl == 0 || rest[0] != '/') {
        path[0] = '/';
        memcpy(path + 1, rest, rl);
        path[rl + 1] = 0;
    } else {
        memcpy(path, rest, rl);
        path[rl] = 0;
    }
    for (const char *q = path; *q; ++q) {
        if ((unsigned char)*q <= 0x20 || (unsigned char)*q >= 0x7F) return false;     // no spaces or raw bytes
    }
    return true;
}

typedef struct {
    rp_sock_t *sock;
    uint8_t    buf[16384];
    size_t     pos, len;
    int64_t    deadline;
} conn_t;

static proven_err_t fill(conn_t *c) {
    int left = (int)(c->deadline - rp_pal_now_ms());
    if (left <= 0) return PROVEN_ERR_AGAIN;
    size_t got = 0;
    proven_err_t err = rp_pal_tcp_recv(c->sock, c->buf, sizeof c->buf, &got, left);
    c->pos = 0;
    c->len = got;
    if (err == PROVEN_OK && got == 0) return PROVEN_ERR_EOF;
    return err;
}

// One byte, or an error (PROVEN_ERR_EOF at the end of the stream).
static proven_err_t get_byte(conn_t *c, uint8_t *b) {
    if (c->pos == c->len) {
        proven_err_t err = fill(c);
        if (err != PROVEN_OK) return err;
    }
    *b = c->buf[c->pos++];
    return PROVEN_OK;
}

// A line without its CRLF (bare LF accepted), at most cap-1 bytes.
static proven_err_t get_line(conn_t *c, char *line, size_t cap, size_t *total) {
    size_t n = 0;
    for (;;) {
        uint8_t b;
        proven_err_t err = get_byte(c, &b);
        if (err != PROVEN_OK) return err;
        if (++*total > MAX_HEAD) return PROVEN_ERR_OUT_OF_BOUNDS;
        if (b == '\n') break;
        if (n + 1 >= cap) return PROVEN_ERR_OUT_OF_BOUNDS;
        line[n++] = (char)b;
    }
    if (n && line[n - 1] == '\r') --n;
    line[n] = 0;
    return PROVEN_OK;
}

static bool ieq_prefix(const char *s, const char *name) {
    size_t n = strlen(name);
    for (size_t i = 0; i < n; ++i) {
        char a = s[i], b = name[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (a != b) return false;
    }
    return s[n] == ':';
}

static const char *value_of(const char *line) {
    const char *v = strchr(line, ':') + 1;
    while (*v == ' ' || *v == '\t') ++v;
    return v;
}

typedef struct {
    int    status;
    bool   chunked, has_length;
    size_t length;
    char   location[2048];
    char   content_type[128];
} head_t;

static proven_err_t read_head(conn_t *c, head_t *h, const char **why) {
    char line[8192];
    size_t total = 0;
    memset(h, 0, sizeof *h);
    proven_err_t err = get_line(c, line, sizeof line, &total);
    if (err != PROVEN_OK) {
        *why = err == PROVEN_ERR_AGAIN ? "the server did not answer in time" : "the server closed the connection without an answer";
        return err;
    }
    if (strncmp(line, "HTTP/1.", 7) != 0 || strlen(line) < 12 || line[8] != ' ') {
        *why = "the server did not answer with HTTP/1.x";
        return PROVEN_ERR_INVALID_FORMAT;
    }
    h->status = (line[9] - '0') * 100 + (line[10] - '0') * 10 + (line[11] - '0');
    for (;;) {
        err = get_line(c, line, sizeof line, &total);
        if (err != PROVEN_OK) {
            *why = err == PROVEN_ERR_OUT_OF_BOUNDS ? "the response headers are too large" : "the response headers are cut off";
            return err;
        }
        if (line[0] == 0) break;
        if (ieq_prefix(line, "content-length")) {
            const char *v = value_of(line);
            size_t n = 0;
            if (*v == 0) goto bad_length;
            for (; *v >= '0' && *v <= '9'; ++v) {
                if (n > (SIZE_MAX - 9) / 10) goto bad_length;
                n = n * 10 + (size_t)(*v - '0');
            }
            if (*v != 0 && *v != ' ' && *v != '\t') goto bad_length;
            if (h->has_length && h->length != n) goto bad_length;
            h->has_length = true;
            h->length = n;
        } else if (ieq_prefix(line, "transfer-encoding")) {
            const char *v = value_of(line);
            if (strcmp(v, "chunked") != 0) {
                *why = "the response uses a transfer coding other than chunked";
                return PROVEN_ERR_UNSUPPORTED;
            }
            h->chunked = true;
        } else if (ieq_prefix(line, "location")) {
            snprintf(h->location, sizeof h->location, "%s", value_of(line));
        } else if (ieq_prefix(line, "content-type")) {
            snprintf(h->content_type, sizeof h->content_type, "%s", value_of(line));
        }
    }
    if (h->chunked && h->has_length) {
        *why = "the response has both Content-Length and chunked (refused: a smuggling risk)";
        return PROVEN_ERR_INVALID_FORMAT;
    }
    return PROVEN_OK;
bad_length:
    *why = "the response has a malformed or conflicting Content-Length";
    return PROVEN_ERR_INVALID_FORMAT;
}

static proven_err_t read_n(conn_t *c, uint8_t *out, size_t n) {
    while (n) {
        if (c->pos == c->len) {
            proven_err_t err = fill(c);
            if (err != PROVEN_OK) return err;
        }
        size_t take = c->len - c->pos < n ? c->len - c->pos : n;
        memcpy(out, c->buf + c->pos, take);
        c->pos += take;
        out += take;
        n -= take;
    }
    return PROVEN_OK;
}

static proven_err_t read_body(proven_allocator_t alloc, conn_t *c, const head_t *h, size_t max, uint8_t **out, size_t *len, const char **why) {
    size_t cap = max, n = 0;
    uint8_t *body = rp_mem_alloc(alloc, cap + 1, 1);
    if (body == NULL) return PROVEN_ERR_NOMEM;
    proven_err_t err = PROVEN_OK;
    if (h->has_length) {
        if (h->length > max) {
            *why = "the response is larger than 4 MiB";
            err = PROVEN_ERR_OUT_OF_BOUNDS;
        } else {
            err = read_n(c, body, h->length);
            n = h->length;
        }
    } else if (h->chunked) {
        for (;;) {
            char line[256];
            size_t total = 0, size = 0;
            err = get_line(c, line, sizeof line, &total);
            if (err != PROVEN_OK) break;
            const char *q = line;
            if (*q == 0) {
                err = PROVEN_ERR_INVALID_FORMAT;
                break;
            }
            for (; *q && *q != ';' && *q != ' '; ++q) {
                int d = *q >= '0' && *q <= '9' ? *q - '0' : *q >= 'a' && *q <= 'f' ? *q - 'a' + 10 : *q >= 'A' && *q <= 'F' ? *q - 'A' + 10 : -1;
                if (d < 0 || size > (SIZE_MAX >> 4)) {
                    err = PROVEN_ERR_INVALID_FORMAT;
                    break;
                }
                size = size * 16 + (size_t)d;
            }
            if (err != PROVEN_OK) break;
            if (size == 0) {                        // trailers until an empty line
                do {
                    err = get_line(c, line, sizeof line, &total);
                } while (err == PROVEN_OK && line[0]);
                break;
            }
            if (size > max - n) {
                *why = "the response is larger than 4 MiB";
                err = PROVEN_ERR_OUT_OF_BOUNDS;
                break;
            }
            err = read_n(c, body + n, size);
            n += size;
            if (err == PROVEN_OK) err = get_line(c, line, sizeof line, &total);
            if (err == PROVEN_OK && line[0]) err = PROVEN_ERR_INVALID_FORMAT;
            if (err != PROVEN_OK) break;
        }
        if (err == PROVEN_ERR_INVALID_FORMAT && *why == NULL) *why = "the chunked response is malformed";
    } else {                                        // until the server closes
        for (;;) {
            if (c->pos == c->len) {
                err = fill(c);
                if (err == PROVEN_ERR_EOF) {
                    err = PROVEN_OK;
                    break;
                }
                if (err != PROVEN_OK) break;
            }
            size_t take = c->len - c->pos;
            if (take > max - n) {
                *why = "the response is larger than 4 MiB";
                err = PROVEN_ERR_OUT_OF_BOUNDS;
                break;
            }
            memcpy(body + n, c->buf + c->pos, take);
            c->pos += take;
            n += take;
        }
    }
    if (err != PROVEN_OK) {
        if (*why == NULL) *why = err == PROVEN_ERR_AGAIN ? "the server did not finish the answer in time" : "the answer was cut off";
        rp_mem_free(alloc, body);
        return err;
    }
    *out = body;
    *len = n;
    return PROVEN_OK;
}

// One exchange with one server: connect, send, read. *head gets the status and headers.
static proven_err_t exchange(proven_allocator_t alloc, const rp_http_req_t *req, const char *host, uint16_t port, const char *path,
                             int64_t deadline, head_t *head, uint8_t **body, size_t *blen, const char **why) {
    conn_t *c = rp_mem_alloc(alloc, 1, sizeof *c);
    if (c == NULL) return PROVEN_ERR_NOMEM;
    memset(c, 0, sizeof *c);
    c->deadline = deadline;
    int left = (int)(deadline - rp_pal_now_ms());
    proven_err_t err = rp_pal_tcp_connect(alloc, host, port, left < CONNECT_MS ? left : CONNECT_MS, &c->sock);
    if (err != PROVEN_OK) {
        *why = err == PROVEN_ERR_NOT_FOUND ? "the server name does not resolve" : err == PROVEN_ERR_AGAIN ? "the server did not accept the connection in time"
                                                                                                        : "the server refused the connection";
        rp_mem_free(alloc, c);
        return err;
    }
    char head_text[4096], hostport[300];
    bool v6 = strchr(host, ':') != NULL;
    if (port == 80) snprintf(hostport, sizeof hostport, "%s%s%s", v6 ? "[" : "", host, v6 ? "]" : "");
    else snprintf(hostport, sizeof hostport, "%s%s%s:%u", v6 ? "[" : "", host, v6 ? "]" : "", (unsigned)port);
    int hn = snprintf(head_text, sizeof head_text,
                      "POST %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: rubrapack/" RUBRAPACK_VERSION_STRING "\r\n"
                      "Content-Type: %s\r\n%s%s%sContent-Length: %zu\r\nConnection: close\r\n\r\n",
                      path, hostport, req->content_type, req->accept ? "Accept: " : "", req->accept ? req->accept : "",
                      req->accept ? "\r\n" : "", req->len);
    if (hn < 0 || (size_t)hn >= sizeof head_text) err = PROVEN_ERR_OUT_OF_BOUNDS;
    if (err == PROVEN_OK) err = rp_pal_tcp_send(c->sock, (const uint8_t *)head_text, (size_t)hn, (int)(deadline - rp_pal_now_ms()));
    if (err == PROVEN_OK) err = rp_pal_tcp_send(c->sock, req->body, req->len, (int)(deadline - rp_pal_now_ms()));
    if (err != PROVEN_OK && *why == NULL) *why = "the request could not be sent";
    if (err == PROVEN_OK) err = read_head(c, head, why);
    if (err == PROVEN_OK) err = read_body(alloc, c, head, req->max_body ? req->max_body : DEFAULT_BODY, body, blen, why);
    rp_pal_tcp_close(c->sock);
    rp_mem_free(alloc, c);
    return err;
}

proven_err_t rp_http_post(proven_allocator_t alloc, const rp_http_req_t *req, rp_http_resp_t *resp, const char **why) {
    memset(resp, 0, sizeof *resp);
    *why = NULL;
    int64_t deadline = rp_pal_now_ms() + (req->total_ms ? req->total_ms : TOTAL_MS);
    char url[2048];
    snprintf(url, sizeof url, "%s", req->url);
    bool was_https = false;
    for (int redirects = 0;; ++redirects) {
        bool https;
        char host[256], path[2048];
        uint16_t port;
        if (!rp_url_parse(url, &https, host, sizeof host, &port, path, sizeof path)) {
            *why = "not an http:// or https:// URL rubrapack accepts";
            return PROVEN_ERR_INVALID_ARG;
        }
        if (was_https && !https) {
            *why = "the server redirected from https to http (refused)";
            return PROVEN_ERR_PERMISSION;
        }
        was_https = https;
        if (https) {
            *why = "https timestamp servers need TLS, which comes with RFC-0008 step 3; use an http:// server for now";
            return PROVEN_ERR_UNSUPPORTED;
        }
        head_t head;
        uint8_t *body = NULL;
        size_t blen = 0;
        proven_err_t err = PROVEN_OK;
        for (int attempt = 0; attempt <= RETRIES; ++attempt) {
            *why = NULL;
            err = exchange(alloc, req, host, port, path, deadline, &head, &body, &blen, why);
            bool again = (err == PROVEN_ERR_AGAIN || err == PROVEN_ERR_IO || err == PROVEN_ERR_EOF || (err == PROVEN_OK && head.status >= 500)) &&
                         rp_pal_now_ms() < deadline;
            if (!again || attempt == RETRIES) break;
            if (err == PROVEN_OK) rp_mem_free(alloc, body);
        }
        if (err != PROVEN_OK) return err;
        if (head.status == 301 || head.status == 302 || head.status == 307 || head.status == 308) {
            rp_mem_free(alloc, body);
            if (redirects == MAX_REDIRECTS) {
                *why = "more than 3 redirects";
                return PROVEN_ERR_OUT_OF_BOUNDS;
            }
            if (head.location[0] == '/') {          // a path on the same server
                char base[512];
                snprintf(base, sizeof base, "%s://%s%s%s:%u", https ? "https" : "http", strchr(host, ':') ? "[" : "", host,
                         strchr(host, ':') ? "]" : "", (unsigned)port);
                if (strlen(base) + strlen(head.location) >= sizeof url) {
                    *why = "the redirect location is too long";
                    return PROVEN_ERR_OUT_OF_BOUNDS;
                }
                memcpy(url, base, strlen(base));
                memcpy(url + strlen(base), head.location, strlen(head.location) + 1);
            } else {
                snprintf(url, sizeof url, "%s", head.location);
            }
            continue;
        }
        resp->status = head.status;
        resp->body = body;
        resp->len = blen;
        snprintf(resp->content_type, sizeof resp->content_type, "%s", head.content_type);
        return PROVEN_OK;
    }
}
