// src/net/tls.c - the TLS 1.3 client for https timestamp servers (include/rubrapack/tls.h): a
// thin layer over proven_c_lib's TLS engine (proven/tls.h, proven/cert.h), which does the
// handshake, the records and the certificate checks. This layer turns the caller's trust anchors
// into a proven certificate store, keeps the received application data until it is read, and
// gives each failure the sentence rubrapack has always given for it.

#include "rubrapack/tls.h"
#include "rubrapack/lang.h"

#include "rubrapack/buf.h"
#include "rubrapack/mem.h"

#include <stdio.h>
#include <string.h>

#include "proven/cert.h"
#include "proven/tls.h"

struct rp_tls {
    proven_allocator_t   alloc;
    proven_cert_store_t *store;
    proven_tls_config_t *config;
    proven_tls_conn_t   *conn;
    char                 host[256];
    int64_t              now;
    bool                 eof, failed;
    rp_buf_t             app;            // application data received and not read yet
    size_t               app_pos;
};

// The last failure's sentence when it names something. Not in rp_tls_t: the caller reports it after
// rp_tls_free (as rubrapack's own client kept it, in static storage).
static thread_local char msg[400];

static proven_mem_view_t mv(const void *p, size_t n) { return (proven_mem_view_t){ (const proven_byte_t *)p, n }; }

static proven_i64 clock_of(void *ctx) { return ((const rp_tls_t *)ctx)->now; }

// The alert names RFC 8446 6 gives the ones a server sends when it will not talk to us.
static const char *alert_name(int a) {
    return a == 40 ? "handshake_failure" : a == 70 ? "protocol_version" : a == 42 ? "bad_certificate" : a == 50 ? "decode_error"
         : a == 47 ? "illegal_parameter" : a == 80 ? "internal_error" : a == 112 ? "unrecognized_name" : a == 71 ? "insufficient_security"
                                                                                                           : "";
}

// The sentence for a failure of proven_tls_feed.
static const char *why_of(rp_tls_t *t, proven_err_t err) {
    t->failed = true;
    if (err == PROVEN_ERR_NOMEM) return "out of memory";
    switch (proven_tls_peer_fault(t->conn)) {
    case PROVEN_CERT_FAULT_NONE: break;
    case PROVEN_CERT_FAULT_EXPIRED:
    case PROVEN_CERT_FAULT_NOT_YET_VALID: return "a certificate in the path is expired or not valid yet";
    case PROVEN_CERT_FAULT_NAME_MISMATCH:
        (void)proven_fmt_to(msg, sizeof msg, "the server's certificate is not for '{}' (subjectAltName)", proven_arg_cstr(t->host));
        return msg;
    case PROVEN_CERT_FAULT_USAGE: return TR_N("the server's certificate is not for TLS servers (its extended key usage)");
    case PROVEN_CERT_FAULT_MALFORMED: return "the server's certificate is malformed";
    default: return "the server's certificate path does not reach a trusted certificate (--tls-trust, --system-roots)";
    }
    int got = proven_tls_alert_received(t->conn);
    if (got == 116) return "the server asks for a client certificate, which rubrapack does not send";     // certificate_required
    if (got > 0) {
        const char *name = alert_name(got);
        bool early = !proven_tls_is_established(t->conn) && (got == 40 || got == 70 || got == 71);
        (void)proven_fmt_to(msg, sizeof msg, "the server ended the handshake with TLS alert {}{}{}{}{}", proven_arg_u32((unsigned)got), proven_arg_cstr(*name ? " (" : ""), proven_arg_cstr(name), proven_arg_cstr(*name ? ")" : ""), proven_arg_cstr(early ? " - it may not speak TLS 1.3, which rubrapack needs" : ""));
        return msg;
    }
    if (proven_tls_alert_sent(t->conn) == 70) return "the server does not speak TLS 1.3 (TLS 1.2 and older are not supported)";
    if (err == PROVEN_ERR_UNTRUSTED) return "the server's certificate path does not reach a trusted certificate (--tls-trust, --system-roots)";
    (void)proven_fmt_to(msg, sizeof msg, "the server broke the TLS protocol (alert {})", proven_arg_i32(proven_tls_alert_sent(t->conn)));
    return msg;
}

proven_err_t rp_tls_new(proven_allocator_t alloc, const rp_tls_config_t *cfg, rp_tls_t **out, const char **why) {
    *out = NULL;
    size_t hl = cfg->host ? strlen(cfg->host) : 0;
    if (hl == 0 || hl >= 256) {
        *why = TR_N("no server name, or one longer than 255 bytes");
        return PROVEN_ERR_INVALID_ARG;
    }
    if (cfg->anchor_count == 0) {
        *why = "no trust anchors for the server (--tls-trust or --system-roots)";
        return PROVEN_ERR_PERMISSION;
    }
    rp_tls_t *t = rp_mem_alloc(alloc, 1, sizeof *t);
    if (t == NULL) return PROVEN_ERR_NOMEM;
    memset(t, 0, sizeof *t);
    t->alloc = alloc;
    memcpy(t->host, cfg->host, hl);
    if (t->host[hl - 1] == '.') t->host[--hl] = 0;      // "example.com." names example.com
    t->now = cfg->now;
    t->app = rp_buf_new(alloc, 1u << 24);
    proven_err_t err = proven_cert_store_create(alloc, &t->store);
    size_t added = 0;
    for (size_t i = 0; err == PROVEN_OK && i < cfg->anchor_count; ++i) {
        // A system bundle may hold a certificate proven does not read; the others still count.
        proven_err_t e = proven_cert_store_add_der(t->store, mv(cfg->anchors[i].p, cfg->anchors[i].n));
        if (e == PROVEN_OK) ++added;
        else if (e == PROVEN_ERR_NOMEM) err = e;
    }
    if (err == PROVEN_OK && added == 0) {
        rp_tls_free(t);
        *why = "no trust anchors for the server (--tls-trust or --system-roots)";
        return PROVEN_ERR_PERMISSION;
    }
    proven_tls_options_t o = {
        .alloc = alloc, .anchors = t->store, .verify = PROVEN_TLS_VERIFY_CHAIN, .now = clock_of, .now_ctx = t,
        .no_resumption = true, .min_version = PROVEN_TLS_VERSION_1_3, .max_version = PROVEN_TLS_VERSION_1_3,
    };
    if (err == PROVEN_OK) err = proven_tls_config_create(&o, &t->config);
    if (err == PROVEN_OK) err = proven_tls_client_create(t->config, (proven_u8str_view_t){ (const proven_byte_t *)t->host, hl }, NULL, &t->conn);
    if (err != PROVEN_OK) {
        rp_tls_free(t);
        *why = err == PROVEN_ERR_NOMEM ? "out of memory" : TR_N("no server name, or one longer than 255 bytes");
        return err == PROVEN_ERR_NOMEM ? err : PROVEN_ERR_INVALID_ARG;
    }
    *out = t;
    return PROVEN_OK;
}

void rp_tls_free(rp_tls_t *t) {
    if (t == NULL) return;
    proven_tls_conn_destroy(t->conn);
    proven_tls_config_destroy(t->config);
    proven_cert_store_destroy(t->store);
    rp_buf_free(&t->app);
    proven_allocator_t a = t->alloc;
    rp_wipe(t, sizeof *t);
    rp_mem_free(a, t);
}

size_t rp_tls_pending(rp_tls_t *t, const uint8_t **data) {
    proven_mem_view_t v = proven_tls_pending_output(t->conn);
    *data = v.size ? v.ptr : NULL;
    return v.size;
}

void rp_tls_sent(rp_tls_t *t, size_t n) { proven_tls_output_sent(t->conn, n); }

bool rp_tls_connected(const rp_tls_t *t) { return !t->failed && proven_tls_is_established(t->conn); }
bool rp_tls_eof(const rp_tls_t *t) { return t->eof; }

// Moves what the engine has decrypted into t->app.
static proven_err_t drain(rp_tls_t *t, const char **why) {
    for (;;) {
        uint8_t buf[16384];
        proven_result_size_t r = proven_tls_read(t->conn, (proven_mem_mut_t){ buf, sizeof buf });
        if (r.err == PROVEN_ERR_NEED_MORE) return PROVEN_OK;
        if (r.err == PROVEN_ERR_EOF) {
            t->eof = true;
            return PROVEN_OK;
        }
        if (r.err != PROVEN_OK) return PROVEN_OK;       // not established yet
        rp_buf_put(&t->app, buf, r.value);
        if (t->app.err != PROVEN_OK) {
            t->failed = true;
            *why = "more data than rubrapack keeps";
            return PROVEN_ERR_NOMEM;
        }
    }
}

proven_err_t rp_tls_feed(rp_tls_t *t, const uint8_t *in, size_t len, const char **why) {
    if (t->failed) {
        *why = TR_N("the connection has failed");
        return PROVEN_ERR_INVALID_STATE;
    }
    while (!t->eof) {
        size_t used = 0;
        proven_err_t err = proven_tls_feed(t->conn, mv(in, len), &used);
        if (err != PROVEN_OK) {
            *why = why_of(t, err);
            return PROVEN_ERR_IO;
        }
        in += used;
        len -= used;
        size_t had = t->app.len;
        err = drain(t, why);
        if (err != PROVEN_OK) return err;
        if (len == 0 || (used == 0 && t->app.len == had)) break;
    }
    return PROVEN_OK;
}

proven_err_t rp_tls_write(rp_tls_t *t, const uint8_t *data, size_t len) {
    if (!rp_tls_connected(t)) return PROVEN_ERR_INVALID_STATE;
    proven_result_size_t r = proven_tls_write(t->conn, mv(data, len));
    if (r.err != PROVEN_OK) return r.err;
    // proven takes at most 64 KiB of unsent records at a time; a timestamp request is a few hundred
    // bytes, so one that does not fit is refused rather than queued here.
    return r.value == len ? PROVEN_OK : PROVEN_ERR_NOMEM;
}

size_t rp_tls_read(rp_tls_t *t, uint8_t *buf, size_t cap) {
    size_t n = t->app.len - t->app_pos;
    if (n > cap) n = cap;
    if (n == 0) return 0;                               // (app.data may still be NULL)
    memcpy(buf, t->app.data + t->app_pos, n);
    t->app_pos += n;
    if (t->app_pos == t->app.len) t->app_pos = t->app.len = 0;
    return n;
}

void rp_tls_close(rp_tls_t *t) {
    if (rp_tls_connected(t)) (void)proven_tls_close(t->conn);
}
