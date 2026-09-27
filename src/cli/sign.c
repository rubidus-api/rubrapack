// src/cli/sign.c - `rubrapack sign` and `rubrapack verify` (RFC-0007 S4; RFC-0008; RFC-0001 7, 12.6).
//
// Passwords come only from an environment variable or a file, never from the command line
// (process lists show arguments), and are wiped after use.

#include "rubrapack/diag.h"
#include "rubrapack/inspect.h"
#include "rubrapack/mem.h"
#include "rubrapack/pal.h"
#include "rubrapack/sign.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "proven/heap.h"

enum { MAX_INPUT = 1u << 30, MAX_KEY = 1u << 22 };

static bool is_pe(const uint8_t *d, size_t n) { return n >= 2 && d[0] == 'M' && d[1] == 'Z'; }
static bool is_cfb(const uint8_t *d, size_t n) {
    static const uint8_t sig[8] = { 0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1 };
    return n >= 8 && memcmp(d, sig, 8) == 0;
}

// Loads --key (with its password) and --cert into kf. The exit code, or RP_EXIT_OK.
static int load_key(proven_allocator_t heap, const char *key, const char *cert, const char *pass_env, const char *pass_file,
                    rp_keyfile_t *kf) {
    uint8_t *pass = NULL;
    size_t pass_len = 0;
    if (pass_env) {
        char *v = rp_pal_getenv(heap, pass_env);
        if (v == NULL) {
            rp_diag_error(RP_DIAG_SIGN, "--pass-env %s: the variable is not set", pass_env);
            return RP_EXIT_USAGE;
        }
        pass = (uint8_t *)v;
        pass_len = strlen(v);
    } else if (pass_file) {
        if (rp_pal_read_file(heap, pass_file, 4096, &pass, &pass_len) != PROVEN_OK) {
            rp_diag_error(RP_DIAG_INPUT, "cannot read the password file '%s'", pass_file);
            return RP_EXIT_IO;
        }
        if (pass_len && pass[pass_len - 1] == '\n') --pass_len;     // one closing line break
        if (pass_len && pass[pass_len - 1] == '\r') --pass_len;
    }
    uint8_t *data = NULL;
    size_t len = 0;
    int rc = RP_EXIT_OK;
    const char *why = NULL;
    if (rp_pal_read_file(heap, key, MAX_KEY, &data, &len) != PROVEN_OK) {
        rp_diag_error(RP_DIAG_INPUT, "cannot read the key file '%s'", key);
        rc = RP_EXIT_IO;
    } else {
        proven_err_t err = rp_keyfile_load(heap, data, len, pass, pass_len, kf, &why);
        if (err != PROVEN_OK) {
            rp_diag_error(RP_DIAG_SIGN, "key file '%s': %s", key, why ? why : "unreadable");
            rc = err == PROVEN_ERR_PERMISSION ? RP_EXIT_SIGN : err == PROVEN_ERR_NOMEM ? RP_EXIT_IO : RP_EXIT_SIGN;
        }
        rp_wipe(data, len);
        rp_mem_free(heap, data);
    }
    if (pass) {
        rp_wipe(pass, pass_len);
        rp_mem_free(heap, pass);
    }
    if (rc == RP_EXIT_OK && cert) {
        if (rp_pal_read_file(heap, cert, MAX_KEY, &data, &len) != PROVEN_OK) {
            rp_diag_error(RP_DIAG_INPUT, "cannot read the certificate file '%s'", cert);
            rc = RP_EXIT_IO;
        } else {
            if (rp_keyfile_add_certs(kf, data, len, &why) != PROVEN_OK) {
                rp_diag_error(RP_DIAG_SIGN, "certificate file '%s': %s", cert, why ? why : "unreadable");
                rc = RP_EXIT_SIGN;
            }
            rp_mem_free(heap, data);
        }
        if (rc != RP_EXIT_OK) rp_keyfile_free(kf);
    }
    return rc;
}

// Reads the certificates in `path` into kf (an empty key file with kf->alloc set) and lists them in
// *spans (freed by the caller). The exit code, or RP_EXIT_OK.
static int load_anchors(proven_allocator_t heap, const char *const *paths, size_t n, rp_keyfile_t *kf, rp_der_span_t **spans) {
    *spans = NULL;
    for (size_t i = 0; i < n; ++i) {
        uint8_t *d;
        size_t dn;
        const char *why;
        if (rp_pal_read_file(heap, paths[i], MAX_KEY, &d, &dn) != PROVEN_OK) {
            rp_diag_error(RP_DIAG_INPUT, "cannot read the certificates in '%s'", paths[i]);
            return RP_EXIT_IO;
        }
        proven_err_t err = rp_keyfile_add_certs(kf, d, dn, &why);
        rp_mem_free(heap, d);
        if (err != PROVEN_OK) {
            rp_diag_error(RP_DIAG_INPUT, "cannot read the certificates in '%s': %s", paths[i], why ? why : "unreadable");
            return RP_EXIT_IO;
        }
    }
    if (kf->cert_count == 0) return RP_EXIT_OK;
    *spans = rp_mem_alloc(heap, kf->cert_count, sizeof **spans);
    if (*spans == NULL) return RP_EXIT_IO;
    for (size_t i = 0; i < kf->cert_count; ++i) (*spans)[i] = (rp_der_span_t){ kf->certs[i], kf->cert_len[i] };
    return RP_EXIT_OK;
}

static void add_system_root(void *ctx, const uint8_t *data, size_t len) {
    size_t skipped = 0;
    (void)rp_keyfile_add_roots(ctx, data, len, &skipped);
}

// Adds the operating system's roots to kf and remakes *spans. The exit code, or RP_EXIT_OK.
static int load_system_roots(proven_allocator_t heap, rp_keyfile_t *kf, rp_der_span_t **spans) {
    if (rp_pal_system_roots(heap, add_system_root, kf) != PROVEN_OK || kf->cert_count == 0) {
        rp_diag_error(RP_DIAG_INPUT, "--system-roots: the operating system's root certificates cannot be read");
        return RP_EXIT_IO;
    }
    rp_mem_free(heap, *spans);
    *spans = rp_mem_alloc(heap, kf->cert_count, sizeof **spans);
    if (*spans == NULL) return RP_EXIT_IO;
    for (size_t i = 0; i < kf->cert_count; ++i) (*spans)[i] = (rp_der_span_t){ kf->certs[i], kf->cert_len[i] };
    return RP_EXIT_OK;
}

int rp_sign_bytes(const rp_sign_args_t *a, const char *label, const uint8_t *data, size_t len, uint8_t **out, size_t *out_len) {
    proven_allocator_t heap = proven_heap_allocator();
    bool pe = is_pe(data, len), cfb = is_cfb(data, len);
    if (!pe && !cfb) {
        rp_diag_error(RP_DIAG_SIGN, "'%s' is neither a PE file (.exe, .dll) nor an MSI package", label);
        return RP_EXIT_USAGE;
    }
    // Three trusts, never mixed (RFC-0008 T2): the key's own chain, the TSA's (--tsa-trust), and
    // an https TSA's TLS server (--tls-trust, --system-roots).
    rp_keyfile_t tsa_kf = { .alloc = heap }, tls_kf = { .alloc = heap };
    rp_der_span_t *tsa_anchors = NULL, *tls_anchors = NULL;
    int rc = RP_EXIT_OK;
    if (a->tsa_trust) rc = load_anchors(heap, &a->tsa_trust, 1, &tsa_kf, &tsa_anchors);
    if (rc == RP_EXIT_OK && a->tls_trust) rc = load_anchors(heap, &a->tls_trust, 1, &tls_kf, &tls_anchors);
    if (rc == RP_EXIT_OK && a->system_roots) rc = load_system_roots(heap, &tls_kf, &tls_anchors);
    rp_keyfile_t kf;
    if (rc == RP_EXIT_OK) rc = load_key(heap, a->key, a->cert, a->pass_env, a->pass_file, &kf);
    if (rc != RP_EXIT_OK) {
        rp_mem_free(heap, tsa_anchors);
        rp_mem_free(heap, tls_anchors);
        rp_keyfile_free(&tsa_kf);
        rp_keyfile_free(&tls_kf);
        return rc;
    }
    const char *why = NULL;
    bool external = false;
    int64_t now = (int64_t)time(NULL);
    rp_tsa_t tsa = { .url = a->timestamp, .proxy = a->proxy, .anchors = tsa_anchors, .anchor_count = tsa_kf.cert_count, .tls_anchors = tls_anchors,
                     .tls_anchor_count = tls_kf.cert_count };
    rp_timestamper_t stamper = { rp_tsa_stamp, &tsa };
    const rp_timestamper_t *ts = a->timestamp ? &stamper : NULL;
    proven_err_t err = pe ? rp_pe_sign(heap, data, len, &kf, now, ts, out, out_len, &why)
                          : rp_msi_sign(heap, data, len, &kf, now, ts, a->allow_unsigned_cabs, &external, out, out_len, &why);
    rp_keyfile_free(&kf);
    rp_mem_free(heap, tsa_anchors);
    rp_mem_free(heap, tls_anchors);
    rp_keyfile_free(&tsa_kf);
    rp_keyfile_free(&tls_kf);
    if (err != PROVEN_OK) {
        // A timestamp failure fails the signing (RFC-0008 1.4): the server, its answer, or its time.
        if (a->timestamp && (tsa.failed || tsa.gen_time)) {
            rp_diag_error(RP_DIAG_SIGN, "'%s': timestamp from %s: %s", label, a->timestamp, why ? why : "failed");
            return RP_EXIT_NET;
        }
        rp_diag_error(RP_DIAG_SIGN, "'%s': %s", label, why ? why : "cannot sign");
        return err == PROVEN_ERR_NOMEM ? RP_EXIT_IO : RP_EXIT_SIGN;
    }
    if (external) rp_diag_warning(RP_DIAG_SIGN, "'%s': its external cabinets are not covered by the signature (--allow-unsigned-cabs)", label);
    if (!a->timestamp) rp_diag_warning(RP_DIAG_SIGN, "'%s': no --timestamp: the signature stops being valid when the certificate expires", label);
    return RP_EXIT_OK;
}

int rp_cmd_sign(int argc, char **argv) {
    rp_sign_args_t a = { 0 };
    const char *file = NULL, *out = NULL;
    for (int i = 2; i < argc; ++i) {
        const char *arg = argv[i], *next = i + 1 < argc ? argv[i + 1] : NULL;
        const char **slot = strcmp(arg, "--key") == 0 ? &a.key : strcmp(arg, "--cert") == 0 ? &a.cert : strcmp(arg, "--pass-env") == 0 ? &a.pass_env
                          : strcmp(arg, "--pass-file") == 0 ? &a.pass_file : strcmp(arg, "--timestamp") == 0 ? &a.timestamp
                          : strcmp(arg, "--tsa-trust") == 0 ? &a.tsa_trust : strcmp(arg, "--tls-trust") == 0 ? &a.tls_trust
                          : strcmp(arg, "--proxy") == 0 ? &a.proxy : strcmp(arg, "-o") == 0 ? &out : NULL;
        if (slot && next) {
            *slot = next;
            ++i;
        } else if (strcmp(arg, "--allow-unsigned-cabs") == 0) {
            a.allow_unsigned_cabs = true;
        } else if (strcmp(arg, "--system-roots") == 0) {
            a.system_roots = true;
        } else if (strcmp(arg, "--pass") == 0 || strncmp(arg, "--pass=", 7) == 0) {
            rp_diag_error(RP_DIAG_EXTRA_ARGUMENT, "a password is never taken on the command line (others can see it); use --pass-env or --pass-file");
            return RP_EXIT_USAGE;
        } else if (arg[0] == '-' || file) {
            file = NULL;
            break;
        } else {
            file = arg;
        }
    }
    if (file == NULL || a.key == NULL || (a.pass_env && a.pass_file) || ((a.tsa_trust || a.tls_trust || a.system_roots || a.proxy) && !a.timestamp)) {
        rp_diag_error(RP_DIAG_EXTRA_ARGUMENT, "usage: rubrapack sign <file.exe|.dll|.msi> --key <key.pfx|.pem> [--cert <chain.pem>] [--pass-env VAR | --pass-file FILE] [--timestamp <URL> [--tsa-trust <certificates>] [--tls-trust <certificates>] [--system-roots] [--proxy <URL>]] [--allow-unsigned-cabs] [-o <out>]");
        return RP_EXIT_USAGE;
    }
    proven_allocator_t heap = proven_heap_allocator();
    uint8_t *data = NULL, *signed_data = NULL;
    size_t len = 0, signed_len = 0;
    if (rp_pal_read_file(heap, file, MAX_INPUT, &data, &len) != PROVEN_OK) {
        rp_diag_error(RP_DIAG_INPUT, "cannot read '%s'", file);
        return RP_EXIT_IO;
    }
    int rc = rp_sign_bytes(&a, file, data, len, &signed_data, &signed_len);
    rp_mem_free(heap, data);
    if (rc != RP_EXIT_OK) return rc;
    // Written in one step: the original stays as it was if anything fails (RFC-0001 7.1).
    const char *target = out ? out : file;
    proven_err_t err = rp_pal_write_file_atomic(heap, target, signed_data, signed_len);
    rp_mem_free(heap, signed_data);
    if (err != PROVEN_OK) {
        rp_diag_error(RP_DIAG_OUTPUT, "cannot write '%s'", target);
        return RP_EXIT_IO;
    }
    char line[1200];
    snprintf(line, sizeof line, "signed %s\n", target);
    return rp_pal_puts(RP_OUT_STDOUT, line) == PROVEN_OK ? RP_EXIT_OK : RP_EXIT_IO;
}

// "2026-09-27T07:44:31Z" from seconds since 1970 (days to civil date, proleptic Gregorian).
static void iso_time(int64_t t, char out[64]) {
    int64_t days = t / 86400, secs = t % 86400;
    if (secs < 0) {
        secs += 86400;
        --days;
    }
    int64_t z = days + 719468, era = (z >= 0 ? z : z - 146096) / 146097, doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365, doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153, d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9, y = yoe + era * 400 + (m <= 2);
    snprintf(out, 64, "%04lld-%02lld-%02lldT%02lld:%02lld:%02lldZ", (long long)y, (long long)m, (long long)d, (long long)(secs / 3600),
             (long long)(secs / 60 % 60), (long long)(secs % 60));
}

// The time the checks use: now, or RUBRAPACK_TEST_NOW (seconds since 1970) - a test hook, so the
// test harness can check an expired certificate without touching a clock (RFC-0001 12.7).
static int64_t check_time(proven_allocator_t heap) {
    char *v = rp_pal_getenv(heap, "RUBRAPACK_TEST_NOW");
    int64_t t = (int64_t)time(NULL);
    if (v) {
        char *end;
        long long n = strtoll(v, &end, 10);
        if (end != v && *end == '\0') t = n;
        rp_mem_free(heap, v);
    }
    return t;
}

int rp_cmd_verify(int argc, char **argv) {
    const char *file = NULL;
    const char *trust[8], *tsa_trust[8];
    size_t ntrust = 0, ntsa = 0;
    bool system_roots = false;
    for (int i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--trust") == 0 && i + 1 < argc && ntrust < 8) {
            trust[ntrust++] = argv[++i];
        } else if (strcmp(argv[i], "--tsa-trust") == 0 && i + 1 < argc && ntsa < 8) {
            tsa_trust[ntsa++] = argv[++i];
        } else if (strcmp(argv[i], "--system-roots") == 0) {
            system_roots = true;
        } else if (argv[i][0] == '-' || file) {
            file = NULL;
            break;
        } else {
            file = argv[i];
        }
    }
    if (file == NULL) {
        rp_diag_error(RP_DIAG_EXTRA_ARGUMENT, "usage: rubrapack verify <file.exe|.dll|.msi> [--trust <certificates>]... [--system-roots] [--tsa-trust <certificates>]...");
        return RP_EXIT_USAGE;
    }
    proven_allocator_t heap = proven_heap_allocator();
    rp_keyfile_t anchors = { .alloc = heap }, tsa_anchors = { .alloc = heap };
    rp_der_span_t *a = NULL, *ta = NULL;
    uint8_t *data = NULL, *sig = NULL;
    size_t len = 0;
    int rc = load_anchors(heap, trust, ntrust, &anchors, &a);
    if (rc == RP_EXIT_OK && system_roots) rc = load_system_roots(heap, &anchors, &a);      // for the signer's chain only
    if (rc == RP_EXIT_OK) rc = load_anchors(heap, tsa_trust, ntsa, &tsa_anchors, &ta);
    if (rc != RP_EXIT_OK) goto done;
    if (rp_pal_read_file(heap, file, MAX_INPUT, &data, &len) != PROVEN_OK) {
        rp_diag_error(RP_DIAG_INPUT, "cannot read '%s'", file);
        rc = RP_EXIT_IO;
        goto done;
    }
    if (!is_pe(data, len) && !is_cfb(data, len)) {
        rp_diag_error(RP_DIAG_VERIFY, "'%s' is neither a PE file nor an MSI package", file);
        rc = RP_EXIT_USAGE;
        goto done;
    }
    rp_authenticode_check_t r;
    const char *why = NULL;
    if (is_pe(data, len)) rp_pe_verify(data, len, &r, &why);
    else rp_msi_verify(heap, data, len, &r, &sig, &why);
    int64_t now = check_time(heap);

    // The timestamp: its genTime replaces `now` for the signer's chain only when the TSA's own
    // chain is trusted through --tsa-trust (RFC-0001 12.6; RFC-0008 T2: --trust does not count).
    char ts_line[512], when[64];
    const char *ts_why = NULL;
    bool ts_ok = true;
    int64_t at = now;
    if (!r.parsed) {
        snprintf(ts_line, sizeof ts_line, "-");
    } else if (r.ts_bad) {
        ts_ok = false;
        ts_why = "the unsigned attributes are malformed or hold more than one timestamp";
        snprintf(ts_line, sizeof ts_line, "invalid - %s", ts_why);
    } else if (r.ts_legacy) {
        ts_ok = false;
        ts_why = "an old-style Authenticode timestamp (PKCS#9 counterSignature), which rubrapack does not check";
        snprintf(ts_line, sizeof ts_line, "unsupported - %s", ts_why);
    } else if (r.ts_token.n == 0) {
        snprintf(ts_line, sizeof ts_line, "not-present");
    } else {
        rp_tsp_token_t t;
        if (!rp_tsp_token(r.ts_token.p, r.ts_token.n, r.sig.p, r.sig.n, NULL, &t, &ts_why)) {
            ts_ok = false;
            snprintf(ts_line, sizeof ts_line, "invalid - %s", ts_why);
        } else {
            iso_time(t.gen_time, when);
            if (ntsa == 0) {
                snprintf(ts_line, sizeof ts_line, "%s, TSA not-checked (give --tsa-trust)", when);
            } else if (rp_chain_trusted_for(t.tsa_cert, t.certs, ta, tsa_anchors.cert_count, t.gen_time, RP_PURPOSE_TIMESTAMP, &ts_why)) {
                snprintf(ts_line, sizeof ts_line, "%s, TSA trusted", when);
                at = t.gen_time;
            } else {
                ts_ok = false;
                snprintf(ts_line, sizeof ts_line, "%s, TSA untrusted - %s", when, ts_why);
            }
        }
    }

    const char *chain = "not-checked (give --trust or --system-roots)";
    const char *chain_why = NULL;
    bool trusted = false;
    size_t nanchors = anchors.cert_count;
    if (r.parsed && nanchors) {
        trusted = rp_chain_trusted(r.signer_cert, r.certs, a, anchors.cert_count, at, &chain_why);
        chain = trusted ? "trusted" : "untrusted";
    }
    char out[2048];
    snprintf(out, sizeof out,
             "%s\n  structure:  %s\n  digest:     %s\n  signature:  %s\n  chain:      %s%s%s\n  revocation: not-checked\n  timestamp:  %s\n",
             file, r.parsed ? "ok" : "invalid", r.parsed ? (r.digest_ok ? "ok" : "mismatch") : "-",
             r.parsed ? (r.attrs_ok && r.signature_ok ? "ok" : "invalid") : "-", chain, chain_why ? " - " : "", chain_why ? chain_why : "", ts_line);
    bool sig_ok = r.parsed && r.digest_ok && r.attrs_ok && r.signature_ok;
    bool ok = sig_ok && ts_ok && trusted;
    if (!ok) {
        const char *reason = !sig_ok ? (why ? why : "the signature does not verify")
                           : !ts_ok ? ts_why
                           : nanchors ? (chain_why ? chain_why : "untrusted") : "no --trust given: the signature verifies but nothing says whom to trust";
        rp_diag_error(RP_DIAG_VERIFY, "'%s': %s%s", file, !sig_ok || ts_ok ? "" : "timestamp: ", reason);
    }
    rc = rp_pal_puts(RP_OUT_STDOUT, out) == PROVEN_OK ? RP_EXIT_OK : RP_EXIT_IO;
    if (!ok) rc = RP_EXIT_SIGN;
done:
    rp_mem_free(heap, sig);
    rp_mem_free(heap, data);
    rp_mem_free(heap, a);
    rp_mem_free(heap, ta);
    rp_keyfile_free(&anchors);
    rp_keyfile_free(&tsa_anchors);
    return rc;
}
