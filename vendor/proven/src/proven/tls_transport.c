#include "proven/tls.h"
#include "../../platform/proven_sys_mem.h"

#if !defined(PROVEN_FREESTANDING) && !defined(PROVEN_NO_NET)

#include "proven_internal_tls.h"

/*
 * The TLS engine over a transport, as a transport. Everything about TLS is in the engine; what
 * is here is the plumbing: read from the plain transport and feed, take what is pending and
 * write it, with every wait bounded by the caller's deadline.
 */

#define TT_MAGIC ((proven_u32)0x544c5354)
#define TT_READ_CHUNK ((proven_size_t)16 * 1024)

typedef struct {
    proven_u32 magic;
    proven_allocator_t alloc;
    proven_transport_t plain;
    proven_tls_conn_t *conn;
    proven_byte_t *rbuf;                          /* bytes read from `plain` and not yet fed */
    proven_size_t rlen, roff;
    proven_err_t failed;
    /* A client given no session uses the configuration's (RFC-0027): its own copy, kept back when
     * the transport goes, and the server's name it is kept under (behind the copy). */
    const proven_tls_config_t *config;
    proven_tls_session_t *own;
    proven_size_t name_len;
    /* Early data: what was written early, kept until the server has said whether it read it. */
    proven_byte_t *early_copy;
    proven_size_t early_len, early_cap;
    bool early_settled, no_early_next, no_early_now;
    bool deferred;                                /* the ClientHello waits for the first write: it is made knowing whether that write may go early */
} proven_tls_transport_t;

/* Send everything the engine has pending. */
static proven_err_t tt_flush(proven_tls_transport_t *t, proven_net_deadline_t until) {
    for (;;) {
        proven_mem_view_t out = proven_tls_pending_output(t->conn);
        if (out.size == 0) return PROVEN_OK;
        proven_result_size_t w = proven_transport_write(t->plain, out, until);
        if (w.err != PROVEN_OK) return w.err;
        proven_tls_output_sent(t->conn, w.value);
    }
}

/* Feed what is left over from last time; when nothing is, read more. Returns after one step.
 *
 * Bytes are read into a buffer on the stack and fed at once. Only what the engine would not
 * take yet (it stops when application data is waiting to be read) is kept, in an allocation
 * of exactly that size, so that a connection with nothing in flight holds no buffer here. */
static proven_err_t tt_pull(proven_tls_transport_t *t, proven_net_deadline_t until) {
    proven_byte_t chunk[TT_READ_CHUNK];
    const proven_byte_t *data;
    proven_size_t len;
    if (t->roff < t->rlen) {
        data = t->rbuf + t->roff;
        len = t->rlen - t->roff;
    } else {
        proven_result_size_t r = proven_transport_read(t->plain, (proven_mem_mut_t){ .ptr = chunk, .size = sizeof chunk }, until);
        if (r.err != PROVEN_OK) return r.err;
        data = chunk;
        len = r.value;
    }
    proven_size_t used = 0;
    proven_err_t e = proven_tls_feed(t->conn, (proven_mem_view_t){ .ptr = data, .size = len }, &used);
    if (data != chunk) {
        t->roff += used;
        if (t->roff == t->rlen) { t->alloc.free_fn(t->alloc.ctx, t->rbuf); t->rbuf = NULL; t->rlen = 0; t->roff = 0; }
    } else if (used < len && e == PROVEN_OK) {
        proven_result_mem_mut_t m = t->alloc.alloc_fn(t->alloc.ctx, len - used, 1);
        if (m.err != PROVEN_OK) return m.err;
        t->rbuf = m.value.ptr;
        proven_sys_mem_copy(t->rbuf, chunk + used, len - used);
        t->rlen = len - used;
        t->roff = 0;
    }
    return e;
}

/* A client whose ClientHello was held back (RFC-0027) makes it now: offering early data when what
 * is written first may go early, otherwise not - and then completes its handshake here. */
static proven_err_t tt_begin(proven_tls_transport_t *t, bool early, proven_net_deadline_t until) {
    t->deferred = false;
    proven_u8str_view_t name = { .ptr = (const proven_byte_t *)(t->own + 1), .size = t->name_len };
    proven_err_t e = early ? proven_tls_client_create_early_(t->config, name, t->own, &t->conn)
                           : proven_tls_client_create(t->config, name, t->own, &t->conn);
    if (e != PROVEN_OK) { t->conn = NULL; return e; }
    if (early && proven_tls_early_room_(t->conn) > 0) return PROVEN_OK;      /* the write that follows goes with the hello */
    while (e == PROVEN_OK && !proven_tls_is_established(t->conn)) {
        e = tt_flush(t, until);
        if (e == PROVEN_OK) e = tt_pull(t, until);
    }
    proven_err_t fe = tt_flush(t, until);
    return e != PROVEN_OK ? e : fe;
}

/* A client that wrote early data, once its handshake is over (RFC-0027): taken, or refused - the
 * server never read it, and it goes again now, first, under the handshake's keys. The ticket it
 * went on is not offered for early data again. */
static proven_err_t tt_settle(proven_tls_transport_t *t, proven_net_deadline_t until) {
    if (t->early_settled || t->early_len == 0 || !proven_tls_is_established(t->conn)) return PROVEN_OK;
    t->early_settled = true;
    if (t->own && !proven_tls_ticket_stored_(t->conn)) proven_tls_session_early_off_(t->own);   /* a ticket that came since is fresh */
    if (proven_tls_early_(t->conn) != PROVEN_TLS_EARLY_ACCEPTED) {
        for (proven_size_t at = 0; at < t->early_len;) {
            proven_result_size_t w = proven_tls_write(t->conn, (proven_mem_view_t){ .ptr = t->early_copy + at, .size = t->early_len - at });
            if (w.err != PROVEN_OK) return w.err;
            at += w.value;
            proven_err_t e = tt_flush(t, until);
            if (e != PROVEN_OK) return e;
        }
    }
    return PROVEN_OK;
}

static proven_result_size_t tt_read(void *ctx, proven_mem_mut_t dest, proven_net_deadline_t until) {
    proven_tls_transport_t *t = (proven_tls_transport_t *)ctx;
    if (t->deferred && t->failed == PROVEN_OK) {
        proven_err_t e = tt_begin(t, false, until);
        if (e != PROVEN_OK) { t->failed = e; return (proven_result_size_t){ .err = e, .value = 0 }; }
    }
    if (!t->conn) return (proven_result_size_t){ .err = t->failed != PROVEN_OK ? t->failed : PROVEN_ERR_INVALID_STATE, .value = 0 };
    for (;;) {
        proven_result_size_t r = proven_tls_read(t->conn, dest);
        if (r.err != PROVEN_ERR_NEED_MORE) return r;
        if (t->failed != PROVEN_OK) return (proven_result_size_t){ .err = t->failed, .value = 0 };
        /* The engine may owe the peer something first: an answer to a key update, the end of a
         * handshake whose early data the server refused. */
        proven_err_t e = tt_flush(t, until);
        if (e == PROVEN_OK) e = tt_settle(t, until);
        if (e == PROVEN_OK) e = tt_pull(t, until);
        if (e == PROVEN_OK) e = tt_settle(t, until);
        if (e != PROVEN_OK) {
            /* A peer that goes away without closing the TLS session has cut it short: that is
             * not an end of stream a caller may trust. */
            if (e == PROVEN_ERR_EOF) e = PROVEN_ERR_RESET;
            if (e != PROVEN_ERR_TIMEOUT && e != PROVEN_ERR_AGAIN) {
                t->failed = e;
                (void)tt_flush(t, until);         /* the alert, if the engine made one */
            }
            return (proven_result_size_t){ .err = e, .value = 0 };
        }
    }
}

static proven_result_size_t tt_write(void *ctx, proven_mem_view_t src, proven_net_deadline_t until) {
    proven_tls_transport_t *t = (proven_tls_transport_t *)ctx;
    if (t->failed != PROVEN_OK) return (proven_result_size_t){ .err = t->failed, .value = 0 };
    if (t->deferred) {
        proven_err_t e = tt_begin(t, !t->no_early_now, until);
        if (e != PROVEN_OK) { t->failed = e; return (proven_result_size_t){ .err = e, .value = 0 }; }
    }
    proven_size_t done = 0;
    if (!proven_tls_is_established(t->conn) && !proven_tls_is_server_(t->conn)) {
        /* A client whose handshake is still going (RFC-0027): what the ticket allows goes as
         * early data, kept; the rest after the handshake, which this write then completes. */
        proven_size_t room = proven_tls_early_room_(t->conn);
        if (room > 0 && !t->early_settled && !t->no_early_now) {
            proven_size_t n = src.size < room ? src.size : room;
            if (t->early_len + n > t->early_cap) {
                proven_size_t cap = t->early_len + n;
                proven_result_mem_mut_t m = t->early_copy ? t->alloc.realloc_fn(t->alloc.ctx, t->early_copy, t->early_cap, cap, 1) : t->alloc.alloc_fn(t->alloc.ctx, cap, 1);
                if (m.err != PROVEN_OK) return (proven_result_size_t){ .err = m.err, .value = 0 };
                t->early_copy = m.value.ptr; t->early_cap = cap;
            }
            proven_result_size_t w = proven_tls_write(t->conn, (proven_mem_view_t){ .ptr = src.ptr, .size = n });
            if (w.err != PROVEN_OK) return w;
            proven_sys_mem_copy(t->early_copy + t->early_len, src.ptr, w.value);
            t->early_len += w.value;
            done = w.value;
            proven_err_t e = tt_flush(t, until);
            if (e != PROVEN_OK) { if (e != PROVEN_ERR_TIMEOUT && e != PROVEN_ERR_AGAIN) t->failed = e; return (proven_result_size_t){ .err = done > 0 ? PROVEN_OK : e, .value = done }; }
            if (done == src.size) return (proven_result_size_t){ .err = PROVEN_OK, .value = done };
        }
        proven_err_t e = PROVEN_OK;
        while (e == PROVEN_OK && !proven_tls_is_established(t->conn)) {
            e = tt_flush(t, until);
            if (e == PROVEN_OK) e = tt_pull(t, until);
        }
        if (e == PROVEN_OK) e = tt_flush(t, until);
        if (e == PROVEN_OK) e = tt_settle(t, until);
        if (e != PROVEN_OK) {
            if (e != PROVEN_ERR_TIMEOUT && e != PROVEN_ERR_AGAIN) t->failed = e;
            return (proven_result_size_t){ .err = done > 0 ? PROVEN_OK : e, .value = done };
        }
    } else if (!t->early_settled && t->early_len > 0) {
        proven_err_t e = tt_settle(t, until);
        if (e != PROVEN_OK) { t->failed = e; return (proven_result_size_t){ .err = e, .value = 0 }; }
    }
    while (done < src.size) {
        proven_result_size_t w = proven_tls_write(t->conn, (proven_mem_view_t){ .ptr = src.ptr + done, .size = src.size - done });
        if (w.err != PROVEN_OK) return (proven_result_size_t){ .err = w.err, .value = done };
        done += w.value;
        proven_err_t e = tt_flush(t, until);
        if (e != PROVEN_OK) {
            if (e != PROVEN_ERR_TIMEOUT && e != PROVEN_ERR_AGAIN) t->failed = e;
            return (proven_result_size_t){ .err = done > 0 ? PROVEN_OK : e, .value = done };
        }
    }
    return (proven_result_size_t){ .err = PROVEN_OK, .value = done };
}

static proven_err_t tt_shutdown(void *ctx) {
    proven_tls_transport_t *t = (proven_tls_transport_t *)ctx;
    if (t->failed != PROVEN_OK) return t->failed;
    if (!t->conn) return proven_transport_shutdown(t->plain);     /* nothing of TLS was sent */
    proven_err_t e = proven_tls_close(t->conn);
    if (e == PROVEN_OK) e = tt_flush(t, proven_net_deadline_in(2000));
    return e;
}

static void tt_free(proven_tls_transport_t *t) {
    proven_allocator_t a = t->alloc;
    if (t->conn) proven_tls_conn_destroy(t->conn);
    if (t->rbuf) a.free_fn(a.ctx, t->rbuf);
    if (t->early_copy) { proven_mem_wipe((proven_mem_mut_t){ .ptr = t->early_copy, .size = t->early_cap }); a.free_fn(a.ctx, t->early_copy); }
    if (t->own) {
        /* The newest session goes back to the configuration for the next connection. */
        if (t->no_early_next) proven_tls_session_early_off_(t->own);
        proven_tls_client_cache_store_(t->config, (proven_u8str_view_t){ .ptr = (const proven_byte_t *)(t->own + 1), .size = t->name_len }, t->own);
        proven_mem_wipe((proven_mem_mut_t){ .ptr = (proven_byte_t *)t->own, .size = sizeof *t->own });
        a.free_fn(a.ctx, t->own);
    }
    t->magic = 0;
    a.free_fn(a.ctx, t);
}

static proven_err_t tt_close(void *ctx) {
    proven_tls_transport_t *t = (proven_tls_transport_t *)ctx;
    /* Say goodbye if the session is still good; do not wait long for a peer that is not reading. */
    if (t->conn && t->failed == PROVEN_OK && proven_tls_close(t->conn) == PROVEN_OK) (void)tt_flush(t, proven_net_deadline_in(1000));
    proven_err_t e = proven_transport_close(t->plain);
    tt_free(t);
    return e;
}

static proven_err_t tt_open(proven_transport_t plain, const proven_tls_config_t *config, proven_tls_conn_t *conn,
                            proven_tls_session_t *own, proven_size_t name_len, proven_net_deadline_t until, proven_transport_t *out) {
    proven_allocator_t a = config->alloc;
    proven_result_mem_mut_t m = a.alloc_fn(a.ctx, sizeof(proven_tls_transport_t), 16);
    if (m.err != PROVEN_OK) { proven_tls_conn_destroy(conn); if (own) a.free_fn(a.ctx, own); return m.err; }
    proven_tls_transport_t *t = (proven_tls_transport_t *)m.value.ptr;
    *t = (proven_tls_transport_t){ .magic = TT_MAGIC, .alloc = a, .plain = plain, .conn = conn, .failed = PROVEN_OK, .config = config, .own = own, .name_len = name_len };
    proven_err_t e = PROVEN_OK;
    while (e == PROVEN_OK && !proven_tls_is_established(conn)) {
        e = tt_flush(t, until);
        if (e == PROVEN_OK) e = tt_pull(t, until);
    }
    /* The last flight (a client's Finished, a server's ticket), or the alert on failure. */
    proven_err_t fe = tt_flush(t, until);
    if (e == PROVEN_OK) e = fe;
    if (e != PROVEN_OK) { tt_free(t); return e; }               /* `plain` stays open: it is the caller's */
    out->ctx = t;
    out->read_fn = tt_read;
    out->write_fn = tt_write;
    out->shutdown_fn = tt_shutdown;
    out->close_fn = tt_close;
    return PROVEN_OK;
}

proven_err_t proven_tls_transport_client(proven_transport_t plain, const proven_tls_config_t *config,
                                         proven_u8str_view_t server_name, proven_tls_session_t *session,
                                         proven_net_deadline_t until, proven_transport_t *out) {
    if (!out || !config || !proven_transport_is_valid(plain) || server_name.size > 253 || (server_name.size > 0 && !server_name.ptr)) return PROVEN_ERR_INVALID_ARG;
    /* Given no session: the configuration's newest for this server (RFC-0027), and early data
     * where its ticket allows it and the configuration asks for it. */
    proven_tls_session_t *own = NULL;
    if (!session && config->client_cache && server_name.size > 0) {
        proven_result_mem_mut_t m = config->alloc.alloc_fn(config->alloc.ctx, sizeof *own + server_name.size, 16);
        if (m.err != PROVEN_OK) return m.err;
        own = (proven_tls_session_t *)(void *)m.value.ptr;
        proven_tls_client_cache_load_(config, server_name, own);
        proven_sys_mem_copy((proven_byte_t *)(own + 1), server_name.ptr, server_name.size);
        session = own;
    }
    if (own && config->early_data && proven_tls_session_allows_early_(own)) {
        /* Nothing is sent yet: the ClientHello goes with the first write, offering early data
         * when that write may go early (RFC-0027). */
        proven_result_mem_mut_t m = config->alloc.alloc_fn(config->alloc.ctx, sizeof(proven_tls_transport_t), 16);
        if (m.err != PROVEN_OK) { config->alloc.free_fn(config->alloc.ctx, own); return m.err; }
        proven_tls_transport_t *t = (proven_tls_transport_t *)m.value.ptr;
        *t = (proven_tls_transport_t){ .magic = TT_MAGIC, .alloc = config->alloc, .plain = plain, .failed = PROVEN_OK, .config = config,
                                       .own = own, .name_len = server_name.size, .deferred = true };
        out->ctx = t; out->read_fn = tt_read; out->write_fn = tt_write; out->shutdown_fn = tt_shutdown; out->close_fn = tt_close;
        return PROVEN_OK;
    }
    proven_tls_conn_t *conn = NULL;
    proven_err_t e = proven_tls_client_create(config, server_name, session, &conn);
    if (e != PROVEN_OK) { if (own) config->alloc.free_fn(config->alloc.ctx, own); return e; }
    return tt_open(plain, config, conn, own, own ? server_name.size : 0, until, out);
}

proven_err_t proven_tls_transport_server(proven_transport_t plain, const proven_tls_config_t *config,
                                         proven_net_deadline_t until, proven_transport_t *out) {
    if (!out || !config || !proven_transport_is_valid(plain)) return PROVEN_ERR_INVALID_ARG;
    proven_tls_conn_t *conn = NULL;
    proven_err_t e = proven_tls_server_create(config, &conn);
    if (e != PROVEN_OK) return e;
    return tt_open(plain, config, conn, NULL, 0, until, out);
}

proven_err_t proven_tls_transport_server_lazy_(proven_transport_t plain, const proven_tls_config_t *config, proven_transport_t *out) {
    /* (A server made here takes early data only when its owner asks: proven_tls_server_take_early_.) */
    proven_tls_conn_t *conn = NULL;
    proven_err_t e = proven_tls_server_create(config, &conn);
    if (e != PROVEN_OK) return e;
    proven_allocator_t a = config->alloc;
    proven_result_mem_mut_t m = a.alloc_fn(a.ctx, sizeof(proven_tls_transport_t), 16);
    if (m.err != PROVEN_OK) { proven_tls_conn_destroy(conn); return m.err; }
    proven_tls_transport_t *t = (proven_tls_transport_t *)m.value.ptr;
    *t = (proven_tls_transport_t){ .magic = TT_MAGIC, .alloc = a, .plain = plain, .conn = conn, .failed = PROVEN_OK, .config = config };
    out->ctx = t; out->read_fn = tt_read; out->write_fn = tt_write; out->shutdown_fn = tt_shutdown; out->close_fn = tt_close;
    return PROVEN_OK;
}

void proven_tls_transport_set_plain_(proven_transport_t tls, proven_transport_t plain) {
    ((proven_tls_transport_t *)tls.ctx)->plain = plain;
}

bool proven_tls_transport_buffered_(proven_transport_t tls) {
    proven_tls_transport_t *t = (proven_tls_transport_t *)tls.ctx;
    if (t->failed != PROVEN_OK) return false;
    if (t->roff < t->rlen) return true;
    proven_byte_t probe;
    proven_result_size_t r = proven_tls_read(t->conn, (proven_mem_mut_t){ .ptr = &probe, .size = 0 });
    return r.err == PROVEN_OK;                    /* plaintext is waiting; a zero-length read takes none */
}

void proven_tls_transport_shutdown_now_(proven_transport_t tls) {
    proven_tls_transport_t *t = (proven_tls_transport_t *)tls.ctx;
    if (t->failed == PROVEN_OK && proven_tls_close(t->conn) == PROVEN_OK) (void)tt_flush(t, PROVEN_NET_DONT_WAIT);
}

void proven_tls_transport_close_now_(proven_transport_t tls) {
    proven_tls_transport_t *t = (proven_tls_transport_t *)tls.ctx;
    proven_tls_transport_shutdown_now_(tls);
    (void)proven_transport_close(t->plain);
    tt_free(t);
}

int proven_tls_transport_early_(proven_transport_t tls) {
    proven_tls_conn_t *c = proven_tls_transport_conn(tls);
    return c ? proven_tls_early_(c) : PROVEN_TLS_EARLY_NONE;
}

static proven_tls_transport_t *tt_of(proven_transport_t tls) {
    proven_tls_transport_t *t = tls.read_fn == tt_read ? (proven_tls_transport_t *)tls.ctx : NULL;
    return t && t->magic == TT_MAGIC ? t : NULL;
}

void proven_tls_transport_no_early_now_(proven_transport_t tls) {
    proven_tls_transport_t *t = tt_of(tls);
    if (t) t->no_early_now = true;
}

void proven_tls_transport_no_early_next_(proven_transport_t tls) {
    proven_tls_transport_t *t = tt_of(tls);
    if (t) t->no_early_next = true;
}

proven_tls_conn_t *proven_tls_transport_conn(proven_transport_t tls) {
    if (tls.read_fn != tt_read || !tls.ctx) return NULL;
    proven_tls_transport_t *t = (proven_tls_transport_t *)tls.ctx;
    return t->magic == TT_MAGIC ? t->conn : NULL;
}

proven_err_t proven_tls_http_wrap(void *config, proven_transport_t plain, proven_u8str_view_t host,
                                  proven_net_deadline_t until, proven_transport_t *out) {
    return proven_tls_transport_client(plain, (const proven_tls_config_t *)config, host, NULL, until, out);
}

#endif
