#ifndef PROVEN_HTTP_COOKIE_H
#define PROVEN_HTTP_COOKIE_H

#include "proven/types.h"
#include "proven/error.h"
#include "proven/memory.h"
#include "proven/u8str.h"
#include "proven/allocator.h"
#include "proven/time.h"

/**
 * @file http_cookie.h
 * @brief A cookie jar for an HTTP client (RFC 6265), with one deliberate restriction.
 *
 * A jar remembers what servers ask a client to remember and gives it back on later requests
 * to the same place. This one is small, bounded, and owned by you: it holds at most the number
 * of cookies you allow, allocates from the allocator you give it, and touches no file.
 *
 * **Every cookie is host-only.** A server may ask for a cookie to be sent to a whole domain
 * (`Domain=example.com`). Deciding which domains a site may claim needs the public-suffix list
 * - a large table of registry policy that changes monthly and that this library does not
 * carry. Without it, honouring `Domain` would let `evil.co.uk` set a cookie for all of
 * `co.uk`. So a cookie is returned only to the exact host that set it. A `Domain` attribute
 * that names the setting host or a parent of it is accepted and ignored; one that names an
 * unrelated domain makes the cookie be refused, as RFC 6265 requires.
 *
 * What that costs: a login on `www.example.com` is not sent to `api.example.com`. For a client
 * that talks to named services, that is rarely wanted anyway.
 *
 * Not handled: `SameSite` (it is a browser concept), cookie prefixes, persistence to disk.
 */

/** @brief Longest name plus value, in bytes, the jar stores; the common browser limit. */
#define PROVEN_HTTP_COOKIE_MAX_SIZE ((proven_size_t)4096)

/**
 * @brief A cookie jar. Caller-owned: declare one, init it, destroy it.
 *
 * Its fields are exposed only so it can be a plain variable. Do not read or write them, and do
 * not copy a jar that holds cookies: two copies would both free the same memory.
 */
typedef struct {
    proven_allocator_t alloc;
    void *entries;
    proven_size_t count;
    proven_size_t cap;
    proven_u64 clock;       /* a counter: which cookie is oldest */
} proven_http_cookie_jar_t;

/**
 * @brief Make an empty jar that holds at most `max_cookies`.
 * @return PROVEN_ERR_INVALID_ARG for an invalid allocator or a limit of 0; PROVEN_ERR_NOMEM.
 */
[[nodiscard]]
proven_err_t proven_http_cookie_jar_init(proven_http_cookie_jar_t *jar, proven_allocator_t alloc, proven_size_t max_cookies);

/** @brief Free everything the jar holds. Safe on a zeroed jar. */
void proven_http_cookie_jar_destroy(proven_http_cookie_jar_t *jar);

/** @brief Forget every cookie, keeping the jar usable. */
void proven_http_cookie_jar_clear(proven_http_cookie_jar_t *jar);

/** @brief How many cookies the jar holds, expired ones included until they are next looked at. */
[[nodiscard]]
proven_size_t proven_http_cookie_jar_count(const proven_http_cookie_jar_t *jar);

/**
 * @brief Store the cookie a response set.
 *
 * @param host the host the request went to, as it was written in the URL.
 * @param path the request's path (`/a/b`); used when the cookie names none.
 * @param secure whether the request went over an encrypted connection. A cookie marked
 *        `Secure` is refused on a connection that is not.
 * @param set_cookie the value of one `Set-Cookie` header.
 * @param now_wall the current wall-clock time (proven_time_now), for `Expires` and `Max-Age`.
 *
 * A cookie with the same name, host and path replaces the one before it. One that is already
 * expired - `Max-Age=0`, an `Expires` in the past - removes it, which is how a server deletes
 * a cookie. When the jar is full the oldest cookie makes room.
 *
 * @return PROVEN_OK when the header was acted on, stored or deleted.
 *         PROVEN_ERR_INVALID_FORMAT when it is not a cookie (no `=`, an empty name, a control
 *         character) - the header is ignored, as a browser would.
 *         PROVEN_ERR_PERMISSION when the cookie is well formed but may not be set from here:
 *         a `Domain` that is not this host or a parent of it, or `Secure` over plain HTTP.
 *         PROVEN_ERR_OUT_OF_BOUNDS when name plus value pass PROVEN_HTTP_COOKIE_MAX_SIZE.
 *         PROVEN_ERR_NOMEM. In every one of these cases the jar is unchanged.
 */
[[nodiscard]]
proven_err_t proven_http_cookie_jar_store(proven_http_cookie_jar_t *jar, proven_u8str_view_t host, proven_u8str_view_t path,
                                          bool secure, proven_u8str_view_t set_cookie, proven_time_t now_wall);

/**
 * @brief Write the value of the `Cookie` header for a request: `a=1; b=2`.
 *
 * Includes every cookie that was set by `host`, whose path covers `path`, that has not expired,
 * and - if marked `Secure` - only when `secure` is true. Cookies with longer paths come first.
 * `*written` is 0 when there is nothing to send: do not write the header then.
 *
 * Expired cookies found on the way are dropped from the jar.
 *
 * @return PROVEN_ERR_OUT_OF_BOUNDS when `out` is too small; nothing useful is in it then.
 */
[[nodiscard]]
proven_err_t proven_http_cookie_jar_header(proven_http_cookie_jar_t *jar, proven_u8str_view_t host, proven_u8str_view_t path,
                                           bool secure, proven_time_t now_wall,
                                           proven_mem_mut_t out, proven_size_t *written);

#endif /* PROVEN_HTTP_COOKIE_H */
