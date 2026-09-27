#ifndef RUBRAPACK_NET_H
#define RUBRAPACK_NET_H

// include/rubrapack/net.h - the HTTP/1.1 client for timestamp servers (RFC-0008; RFC 9110,
// RFC 9112). One POST, a bounded response, nothing kept between requests.
//
// Limits (RFC-0001 12.4): response body 4 MiB, 10 s to connect, 60 s in all, 3 redirects,
// 2 retries after a failed connection, a timeout or a 5xx answer. Redirects keep the POST
// (301, 302, 307, 308); one from https to http is refused. Content-Length and chunked together,
// or two different Content-Lengths, are refused.

#include <stddef.h>
#include <stdint.h>

#include "proven/allocator.h"
#include "proven/types.h"

typedef struct {
    const char    *url;             // http://host[:port]/path (https follows with TLS, RFC-0008 3)
    const char    *content_type;
    const char    *accept;          // may be NULL
    const uint8_t *body;
    size_t         len;
    const char    *proxy;           // http://host:port or NULL (RFC-0008 T5)
    size_t         max_body;        // 0 = 4 MiB
    int            total_ms;        // 0 = 60000
} rp_http_req_t;

typedef struct {
    int      status;
    uint8_t *body;                  // free with rp_mem_free
    size_t   len;
    char     content_type[128];
} rp_http_resp_t;

// The error kinds a caller turns into exit code 6 (network) or 3.
[[nodiscard]] proven_err_t rp_http_post(proven_allocator_t alloc, const rp_http_req_t *req, rp_http_resp_t *resp, const char **why);

// Parses an http(s) URL. host gets the name (IPv6 literals without brackets); path at least "/".
[[nodiscard]] bool rp_url_parse(const char *url, bool *https, char *host, size_t host_cap, uint16_t *port, char *path, size_t path_cap);

#endif // RUBRAPACK_NET_H
