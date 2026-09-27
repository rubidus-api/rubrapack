// src/cli/sign.c - `rubrapack sign` and `rubrapack verify` (RFC-0007 S4; RFC-0001 7, 12.6).
//
// Passwords come only from an environment variable or a file, never from the command line
// (process lists show arguments), and are wiped after use.

#include "rubrapack/diag.h"
#include "rubrapack/inspect.h"
#include "rubrapack/mem.h"
#include "rubrapack/pal.h"
#include "rubrapack/sign.h"

#include <stdio.h>
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

int rp_cmd_sign(int argc, char **argv) {
    const char *file = NULL, *key = NULL, *cert = NULL, *pass_env = NULL, *pass_file = NULL, *out = NULL;
    for (int i = 2; i < argc; ++i) {
        const char *a = argv[i], *next = i + 1 < argc ? argv[i + 1] : NULL;
        const char **slot = strcmp(a, "--key") == 0 ? &key : strcmp(a, "--cert") == 0 ? &cert : strcmp(a, "--pass-env") == 0 ? &pass_env
                          : strcmp(a, "--pass-file") == 0 ? &pass_file : strcmp(a, "-o") == 0 ? &out : NULL;
        if (slot && next) {
            *slot = next;
            ++i;
        } else if (strcmp(a, "--pass") == 0 || strncmp(a, "--pass=", 7) == 0) {
            rp_diag_error(RP_DIAG_EXTRA_ARGUMENT, "a password is never taken on the command line (others can see it); use --pass-env or --pass-file");
            return RP_EXIT_USAGE;
        } else if (a[0] == '-' || file) {
            file = NULL;
            break;
        } else {
            file = a;
        }
    }
    if (file == NULL || key == NULL || (pass_env && pass_file)) {
        rp_diag_error(RP_DIAG_EXTRA_ARGUMENT, "usage: rubrapack sign <file.exe|.dll> --key <key.pfx|.pem> [--cert <chain.pem>] [--pass-env VAR | --pass-file FILE] [-o <out>]");
        return RP_EXIT_USAGE;
    }
    proven_allocator_t heap = proven_heap_allocator();
    uint8_t *data = NULL;
    size_t len = 0;
    if (rp_pal_read_file(heap, file, MAX_INPUT, &data, &len) != PROVEN_OK) {
        rp_diag_error(RP_DIAG_INPUT, "cannot read '%s'", file);
        return RP_EXIT_IO;
    }
    if (!is_pe(data, len)) {
        if (is_cfb(data, len)) rp_diag_error(RP_DIAG_NOT_IMPLEMENTED, "signing MSI packages is not implemented yet (RFC-0007 step 4)");
        else rp_diag_error(RP_DIAG_SIGN, "'%s' is neither a PE file (.exe, .dll) nor an MSI package", file);
        rp_mem_free(heap, data);
        return RP_EXIT_USAGE;
    }
    rp_keyfile_t kf;
    int rc = load_key(heap, key, cert, pass_env, pass_file, &kf);
    if (rc != RP_EXIT_OK) {
        rp_mem_free(heap, data);
        return rc;
    }
    uint8_t *signed_data = NULL;
    size_t signed_len = 0;
    const char *why = NULL;
    proven_err_t err = rp_pe_sign(heap, data, len, &kf, (int64_t)time(NULL), &signed_data, &signed_len, &why);
    rp_keyfile_free(&kf);
    rp_mem_free(heap, data);
    if (err != PROVEN_OK) {
        rp_diag_error(RP_DIAG_SIGN, "'%s': %s", file, why ? why : "cannot sign");
        return err == PROVEN_ERR_NOMEM ? RP_EXIT_IO : RP_EXIT_SIGN;
    }
    // Written in one step: the original stays as it was if anything fails (RFC-0001 7.1).
    const char *target = out ? out : file;
    err = rp_pal_write_file_atomic(heap, target, signed_data, signed_len);
    rp_mem_free(heap, signed_data);
    if (err != PROVEN_OK) {
        rp_diag_error(RP_DIAG_OUTPUT, "cannot write '%s'", target);
        return RP_EXIT_IO;
    }
    char line[1200];
    snprintf(line, sizeof line, "signed %s\n", target);
    return rp_pal_puts(RP_OUT_STDOUT, line) == PROVEN_OK ? RP_EXIT_OK : RP_EXIT_IO;
}

int rp_cmd_verify(int argc, char **argv) {
    const char *file = NULL;
    const char *trust[8];
    size_t ntrust = 0;
    for (int i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--trust") == 0 && i + 1 < argc && ntrust < 8) {
            trust[ntrust++] = argv[++i];
        } else if (argv[i][0] == '-' || file) {
            file = NULL;
            break;
        } else {
            file = argv[i];
        }
    }
    if (file == NULL) {
        rp_diag_error(RP_DIAG_EXTRA_ARGUMENT, "usage: rubrapack verify <file.exe|.dll> [--trust <certificate>]...");
        return RP_EXIT_USAGE;
    }
    proven_allocator_t heap = proven_heap_allocator();
    rp_keyfile_t anchors = { .alloc = heap };
    for (size_t i = 0; i < ntrust; ++i) {
        uint8_t *d;
        size_t n;
        const char *why;
        if (rp_pal_read_file(heap, trust[i], MAX_KEY, &d, &n) != PROVEN_OK || rp_keyfile_add_certs(&anchors, d, n, &why) != PROVEN_OK) {
            rp_diag_error(RP_DIAG_INPUT, "cannot read the certificates in '%s'", trust[i]);
            rp_keyfile_free(&anchors);
            return RP_EXIT_IO;
        }
        rp_mem_free(heap, d);
    }
    uint8_t *data = NULL;
    size_t len = 0;
    if (rp_pal_read_file(heap, file, MAX_INPUT, &data, &len) != PROVEN_OK) {
        rp_diag_error(RP_DIAG_INPUT, "cannot read '%s'", file);
        rp_keyfile_free(&anchors);
        return RP_EXIT_IO;
    }
    if (!is_pe(data, len)) {
        rp_diag_error(RP_DIAG_NOT_IMPLEMENTED, "verify reads PE files (.exe, .dll) for now; MSI follows with MSI signing");
        rp_mem_free(heap, data);
        rp_keyfile_free(&anchors);
        return RP_EXIT_USAGE;
    }
    rp_authenticode_check_t r;
    const char *why = NULL;
    rp_pe_verify(data, len, &r, &why);
    const char *chain = "not-checked (give --trust)";
    const char *chain_why = NULL;
    bool trusted = false;
    if (r.parsed && ntrust) {
        rp_der_span_t *a = rp_mem_alloc(heap, ntrust * 8 + anchors.cert_count, sizeof *a);
        for (size_t i = 0; a && i < anchors.cert_count; ++i) a[i] = (rp_der_span_t){ anchors.certs[i], anchors.cert_len[i] };
        trusted = a && rp_chain_trusted(r.signer_cert, r.certs, a, anchors.cert_count, (int64_t)time(NULL), &chain_why);
        chain = trusted ? "trusted" : "untrusted";
        rp_mem_free(heap, a);
    }
    char out[2048];
    snprintf(out, sizeof out,
             "%s\n  structure:  %s\n  digest:     %s\n  signature:  %s\n  chain:      %s%s%s\n  revocation: not-checked\n  timestamp:  not-present\n",
             file, r.parsed ? "ok" : "invalid", r.parsed ? (r.digest_ok ? "ok" : "mismatch") : "-",
             r.parsed ? (r.attrs_ok && r.signature_ok ? "ok" : "invalid") : "-", chain, chain_why ? " - " : "", chain_why ? chain_why : "");
    bool ok = r.parsed && r.digest_ok && r.attrs_ok && r.signature_ok && trusted;
    if (!ok) {
        const char *reason = !r.parsed || !r.digest_ok || !r.attrs_ok || !r.signature_ok ? (why ? why : "the signature does not verify")
                           : ntrust ? (chain_why ? chain_why : "untrusted") : "no --trust given: the signature verifies but nothing says whom to trust";
        rp_diag_error(RP_DIAG_VERIFY, "'%s': %s", file, reason);
    }
    rp_mem_free(heap, data);
    rp_keyfile_free(&anchors);
    int rc = rp_pal_puts(RP_OUT_STDOUT, out) == PROVEN_OK ? RP_EXIT_OK : RP_EXIT_IO;
    return ok ? rc : RP_EXIT_SIGN;
}
