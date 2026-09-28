// src/cli/build.c - `rubrapack build <src.rpk> -o <out.msi> [options]` (RFC-0001 7, 7.1).

#include "rubrapack/build.h"
#include "rubrapack/diag.h"
#include "rubrapack/inspect.h"
#include "rubrapack/ir.h"
#include "rubrapack/msix.h"
#include "rubrapack/mem.h"
#include "rubrapack/pal.h"
#include "rubrapack/toml.h"

#include <stdio.h>
#include <string.h>

#include "proven/heap.h"

enum { MAX_DEFINES = 64, MAX_SOURCE = 1u << 24 };

static bool ends_with(const char *s, const char *suffix) {
    size_t n = strlen(s), m = strlen(suffix);
    if (n < m) return false;
    for (size_t k = 0; k < m; ++k) {
        char a = s[n - m + k], b = suffix[k];
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (a != b) return false;
    }
    return true;
}

// A .msixbundle: the source built once per architecture (its own $(ARCH) each time), every package
// named <identity>_<version>_<arch>.msix inside, the architectures in the order given.
static int build_bundle(proven_allocator_t heap, const rp_tdoc_t *doc, const char *dir, const rp_define_t *defines, size_t ndef,
                        char (*archs)[8], size_t narch, bool nfc, const rp_msix_options_t *mopt, const rp_sign_args_t *sign,
                        const char *out, const char *src, rp_srcdiags_t *d) {
    static const char *const arch_names[] = { "x64", "arm64", "x86" };
    rp_msix_part_t parts[3];
    uint8_t *pkgs[3] = { 0 };
    char names[3][320];
    size_t n = 0;
    proven_err_t err = PROVEN_OK;
    for (size_t k = 0; k < (narch ? narch : 1) && err == PROVEN_OK; ++k) {
        rp_ir_t ir;
        rp_ir_options_t opt = { dir, defines, ndef, narch ? archs[k] : NULL, NULL, out, nfc };
        err = rp_ir_build(heap, doc, &opt, &ir, d);
        if (err != PROVEN_OK) break;
        size_t len = 0;
        err = rp_msix_from_ir(heap, &ir, mopt, &pkgs[n], &len, d);
        if (err == PROVEN_OK) {
            unsigned v[4] = { 0 };
            for (size_t i = 0; i < ir.version_count && i < 4; ++i) v[i] = ir.version_parts[i];
            snprintf(names[n], sizeof names[n], "%s_%u.%u.%u.%u_%s.msix", ir.msix_identity_name, v[0], v[1], v[2], v[3], arch_names[ir.arch]);
            parts[n] = (rp_msix_part_t){ names[n], pkgs[n], len };
            ++n;
        }
        rp_ir_free(&ir);
    }
    rp_srcdiag_print(d, src);
    int rc = err == PROVEN_OK ? RP_EXIT_OK : d->errors ? RP_EXIT_SOURCE : RP_EXIT_IO;
    if (err == PROVEN_OK) {
        rp_limits_t lim = rp_limits_default();
        uint8_t *b = NULL;
        size_t bl = 0;
        const char *why = NULL;
        err = rp_msix_bundle(heap, parts, n, &lim, &b, &bl, &why);
        if (err == PROVEN_OK && sign->key) {                // --key: the packages, then the bundle
            uint8_t *sb = NULL;
            size_t sl = 0;
            rc = rp_sign_bytes(sign, out, b, bl, &sb, &sl);
            rp_mem_free(heap, b);
            b = sb;
            bl = sl;
        }
        if (err != PROVEN_OK) {
            rp_diag_error(RP_DIAG_OUTPUT, "cannot bundle the packages: %s", why ? why : "out of memory");
            rc = RP_EXIT_IO;
        } else if (rc != RP_EXIT_OK) {
            // rp_sign_bytes said why
        } else if (rp_pal_write_file_atomic(heap, out, b, bl) != PROVEN_OK) {
            rp_diag_error(RP_DIAG_OUTPUT, "cannot write '%s'", out);
            rc = RP_EXIT_IO;
        }
        rp_mem_free(heap, b);
    }
    for (size_t i = 0; i < n; ++i) rp_mem_free(heap, pkgs[i]);
    return rc;
}

// `build`, or with `lint` set `lint <src.rpk>`: the same steps (parse, model, tables, RP20xx/RP21xx)
// without writing anything (RFC-0006 1). --strict turns warnings into a lint failure.
static int run(int argc, char **argv, bool lint) {
    const char *src = NULL, *out = NULL, *arch = NULL, *compress = NULL;
    bool strict = false, nfc = false;
    rp_sign_args_t sign = { 0 };
    rp_define_t defines[MAX_DEFINES];
    char *define_buf[MAX_DEFINES];
    size_t ndef = 0;
    bool reproducible = false, msix_compress_given = false;
    rp_msix_options_t msix_opt = { 0 };
    const char *target = NULL;
    int rc = RP_EXIT_USAGE;

    for (int i = 2; i < argc; ++i) {
        const char *a = argv[i];
        const char *next = i + 1 < argc ? argv[i + 1] : NULL;
        if (lint && strcmp(a, "--strict") == 0) {
            strict = true;
        } else if (!lint && strcmp(a, "-o") == 0 && next) {
            out = next;
            ++i;
        } else if (strcmp(a, "-D") == 0 && next) {
            const char *eq = strchr(next, '=');
            if (eq == NULL || eq == next || ndef == MAX_DEFINES) {
                rp_diag_error(RP_DIAG_EXTRA_ARGUMENT, "-D needs NAME=VALUE (got '%s')", next);
                goto done;
            }
            size_t nl = (size_t)(eq - next);
            for (size_t k = 0; k < ndef; ++k) {
                if (strlen(defines[k].name) == nl && memcmp(defines[k].name, next, nl) == 0) {
                    rp_diag_error("RP1402", "-D %.*s is given twice", (int)nl, next);
                    goto done;
                }
            }
            char *copy = rp_mem_alloc(proven_heap_allocator(), strlen(next) + 1, 1);
            if (copy == NULL) {
                rc = RP_EXIT_IO;
                goto done;
            }
            strcpy(copy, next);
            copy[nl] = '\0';
            define_buf[ndef] = copy;
            defines[ndef++] = (rp_define_t){ copy, copy + nl + 1 };
            ++i;
        } else if (strcmp(a, "--arch") == 0 && next) {
            arch = next;
            ++i;
        } else if (strcmp(a, "--compress") == 0 && next) {
            compress = next;
            ++i;
        } else if (!lint && (strcmp(a, "--key") == 0 || strcmp(a, "--cert") == 0 || strcmp(a, "--pass-env") == 0 ||
                             strcmp(a, "--pass-file") == 0 || strcmp(a, "--timestamp") == 0 || strcmp(a, "--tsa-trust") == 0 ||
                             strcmp(a, "--tls-trust") == 0 || strcmp(a, "--proxy") == 0) && next) {
            const char **slot = strcmp(a, "--key") == 0 ? &sign.key : strcmp(a, "--cert") == 0 ? &sign.cert
                              : strcmp(a, "--pass-env") == 0 ? &sign.pass_env : strcmp(a, "--pass-file") == 0 ? &sign.pass_file
                              : strcmp(a, "--timestamp") == 0 ? &sign.timestamp : strcmp(a, "--tsa-trust") == 0 ? &sign.tsa_trust
                              : strcmp(a, "--proxy") == 0 ? &sign.proxy : &sign.tls_trust;
            *slot = next;
            ++i;
        } else if (!lint && strcmp(a, "--system-roots") == 0) {
            sign.system_roots = true;
        } else if (!lint && strcmp(a, "--allow-unsigned-cabs") == 0) {
            sign.allow_unsigned_cabs = true;
        } else if (strcmp(a, "--pass") == 0 || strncmp(a, "--pass=", 7) == 0) {
            rp_diag_error(RP_DIAG_EXTRA_ARGUMENT, "a password is never taken on the command line (others can see it); use --pass-env or --pass-file");
            goto done;
        } else if (strcmp(a, "--nfc") == 0) {
            nfc = true;
        } else if (strcmp(a, "--reproducible") == 0) {
            reproducible = true;
        } else if (!lint && strcmp(a, "--unsigned-test") == 0) {
            msix_opt.unsigned_test = true;
        } else if (!lint && strcmp(a, "--msix-compress") == 0 && next) {
            if (strcmp(next, "store") != 0 && strcmp(next, "deflate") != 0) {
                rp_diag_error(RP_DIAG_EXTRA_ARGUMENT, "--msix-compress takes deflate or store (got '%s')", next);
                goto done;
            }
            msix_opt.store = strcmp(next, "store") == 0;
            msix_compress_given = true;
            ++i;
        } else if (strcmp(a, "--target") == 0 && next) {
            if (strcmp(next, "msi") != 0 && strcmp(next, "msix") != 0) {
                rp_diag_error(RP_DIAG_NOT_IMPLEMENTED, "--target %s is not implemented (msi or msix)", next);
                goto done;
            }
            target = next;
            ++i;
        } else if (a[0] == '-' && a[1] != '\0') {
            rp_diag_error(RP_DIAG_EXTRA_ARGUMENT, "unknown or incomplete %s option '%s'", lint ? "lint" : "build", a);
            goto done;
        } else if (src == NULL) {
            src = a;
        } else {
            rp_diag_error(RP_DIAG_EXTRA_ARGUMENT, "more than one source file ('%s')", a);
            goto done;
        }
    }
    if (lint && src) {
        out = "package.msi";        // names the external cabinets only; nothing is written
    }
    if (!lint && (sign.cert || sign.pass_env || sign.pass_file || sign.allow_unsigned_cabs || sign.timestamp) && sign.key == NULL) {
        rp_diag_error(RP_DIAG_EXTRA_ARGUMENT, "--cert, --pass-env, --pass-file, --timestamp and --allow-unsigned-cabs go with --key");
        goto done;
    }
    if (!lint && (sign.tsa_trust || sign.tls_trust || sign.system_roots || sign.proxy) && !sign.timestamp) {
        rp_diag_error(RP_DIAG_EXTRA_ARGUMENT, "--tsa-trust, --tls-trust, --system-roots and --proxy go with --timestamp");
        goto done;
    }
    if (src == NULL || out == NULL) {
        if (lint) rp_diag_error(RP_DIAG_EXTRA_ARGUMENT, "usage: rubrapack lint <src.rpk> [-D NAME=VALUE] [--arch x64|arm64|x86] [--nfc] [--strict]");
        else rp_diag_error(RP_DIAG_EXTRA_ARGUMENT, "usage: rubrapack build <src.rpk> -o <out.msi|out.msix|out.msixbundle> [-D NAME=VALUE] [--arch x64|arm64|x86 (a list for a bundle)] [--compress none] [--nfc] [--reproducible] [--key <key> [--cert <chain.pem>] [--pass-env VAR | --pass-file FILE] [--timestamp <URL> [--tsa-trust <certificates>] [--tls-trust <certificates>] [--system-roots] [--proxy <URL>]] [--allow-unsigned-cabs]] [--unsigned-test] [--msix-compress deflate|store]");
        goto done;
    }
    bool bundle = !lint && ends_with(out, ".msixbundle");
    bool msix = !lint && (bundle || ends_with(out, ".msix"));
    if (!lint && !msix && !ends_with(out, ".msi")) {
        rp_diag_error(RP_DIAG_NOT_IMPLEMENTED, "output '%s': rubrapack writes .msi, .msix and .msixbundle", out);
        goto done;
    }
    // A bundle takes a list of architectures (RFC-0010 P8b-2); everything else one.
    char arch_list[3][8];
    size_t narch = 0;
    if (arch && strchr(arch, ',') && !bundle) {
        rp_diag_error(RP_DIAG_EXTRA_ARGUMENT, "--arch takes one architecture, or a list for a .msixbundle output");
        goto done;
    }
    for (const char *p = arch; bundle && p && *p;) {
        const char *comma = strchr(p, ',');
        size_t n = comma ? (size_t)(comma - p) : strlen(p);
        bool dup = false;
        for (size_t k = 0; k < narch; ++k) dup |= strlen(arch_list[k]) == n && memcmp(arch_list[k], p, n) == 0;
        if (narch == 3 || n == 0 || n >= sizeof arch_list[0] || dup) {
            rp_diag_error(RP_DIAG_EXTRA_ARGUMENT, "--arch for a bundle: a list of x64, x86 and arm64, each once (got '%s')", arch);
            goto done;
        }
        memcpy(arch_list[narch], p, n);
        arch_list[narch++][n] = 0;
        p = comma ? comma + 1 : p + n;
    }
    if (target && strcmp(target, msix ? "msix" : "msi") != 0) {
        rp_diag_error(RP_DIAG_EXTRA_ARGUMENT, "--target %s does not match the output '%s'", target, out);
        goto done;
    }
    if (!msix && (msix_opt.unsigned_test || msix_compress_given)) {
        rp_diag_error(RP_DIAG_EXTRA_ARGUMENT, "--unsigned-test and --msix-compress are for .msix outputs");
        goto done;
    }
    if (msix && compress) {
        rp_diag_error(RP_DIAG_EXTRA_ARGUMENT, "--compress is for .msi; an MSIX takes --msix-compress deflate|store");
        goto done;
    }
    if (msix && sign.key && msix_opt.unsigned_test) {
        rp_diag_error(RP_DIAG_EXTRA_ARGUMENT, "--unsigned-test is for packages without a signature; with --key leave it out");
        goto done;
    }
    if (msix && sign.allow_unsigned_cabs) {
        rp_diag_error(RP_DIAG_EXTRA_ARGUMENT, "--allow-unsigned-cabs is for .msi");
        goto done;
    }

    {
        proven_allocator_t heap = proven_heap_allocator();
        uint8_t *text = NULL;
        size_t text_len = 0;
        proven_err_t err = rp_pal_read_file(heap, src, MAX_SOURCE, &text, &text_len);
        if (err != PROVEN_OK) {
            rp_diag_error(RP_DIAG_INPUT, "cannot read '%s' (%s)", src, err == PROVEN_ERR_NOT_FOUND ? "not found" : "read error");
            rc = RP_EXIT_IO;
            goto done;
        }
        // The source's directory: everything up to the last '/' (or '\' on Windows).
        char dir[1024];
        const char *slash = strrchr(src, '/');
#if defined(_WIN32)
        const char *bslash = strrchr(src, '\\');
        if (bslash && (!slash || bslash > slash)) slash = bslash;
#endif
        if (slash) snprintf(dir, sizeof dir, "%.*s", (int)(slash - src), src);
        else snprintf(dir, sizeof dir, ".");

        rp_srcdiags_t d = { 0 };
        rp_tdoc_t doc;
        rp_ir_t ir;
        err = rp_toml_parse(heap, text, text_len, &doc, &d);
        if (err == PROVEN_OK && bundle) {
            rc = build_bundle(heap, &doc, dir, defines, ndef, arch_list, narch, nfc, &msix_opt, &sign, out, src, &d);
            rp_toml_free(&doc);
            rp_mem_free(heap, text);
            goto done;
        }
        if (err == PROVEN_OK) {
            rp_ir_options_t opt = { dir, defines, ndef, arch, compress, lint ? NULL : out, nfc };
            err = rp_ir_build(heap, &doc, &opt, &ir, &d);
            rp_toml_free(&doc);
        }
        rp_mem_free(heap, text);
        if (err == PROVEN_OK && msix) {
            uint8_t *pkg = NULL;
            size_t pkg_len = 0;
            err = rp_msix_from_ir(heap, &ir, &msix_opt, &pkg, &pkg_len, &d);
            rp_ir_free(&ir);
            int sign_rc = RP_EXIT_OK;
            if (err == PROVEN_OK && sign.key) {             // --key: signed before anything is written
                uint8_t *signed_pkg = NULL;
                size_t signed_len = 0;
                sign_rc = rp_sign_bytes(&sign, out, pkg, pkg_len, &signed_pkg, &signed_len);
                rp_mem_free(heap, pkg);
                pkg = signed_pkg;
                pkg_len = signed_len;
            }
            if (err == PROVEN_OK && sign_rc == RP_EXIT_OK) {
                err = rp_pal_write_file_atomic(heap, out, pkg, pkg_len);
                if (err != PROVEN_OK) rp_diag_error(RP_DIAG_OUTPUT, "cannot write '%s'", out);
            }
            rp_mem_free(heap, pkg);
            rp_srcdiag_print(&d, src);
            rc = sign_rc != RP_EXIT_OK ? sign_rc : err == PROVEN_OK ? RP_EXIT_OK : d.errors ? RP_EXIT_SOURCE : RP_EXIT_IO;
        } else if (err == PROVEN_OK) {
            rp_limits_t limits = rp_limits_default();
            // External cabinets are named after the package: <stem>.cab next to <stem>.msi.
            char stem[512], cabpath[1536];
            const char *base = strrchr(out, '/');
#if defined(_WIN32)
            const char *bs = strrchr(out, '\\');
            if (bs && (!base || bs > base)) base = bs;
#endif
            base = base ? base + 1 : out;
            snprintf(stem, sizeof stem, "%.*s", (int)(strlen(base) - 4), base);
            size_t outdir = (size_t)(base - out);
            rp_build_options_t bopt = { reproducible, stem };
            uint8_t *msi = NULL;
            size_t msi_len = 0;
            rp_build_file_t *cabs = NULL;
            size_t ncabs = 0;
            err = rp_msi_from_ir(heap, &ir, &bopt, &limits, &msi, &msi_len, &cabs, &ncabs, &d);
            rp_ir_free(&ir);
            // --key: signed before anything is written (the same code as `rubrapack sign`).
            if (err == PROVEN_OK && !lint && sign.key) {
                uint8_t *signed_msi = NULL;
                size_t signed_len = 0;
                int src_rc = rp_sign_bytes(&sign, out, msi, msi_len, &signed_msi, &signed_len);
                if (src_rc != RP_EXIT_OK) {
                    rp_build_files_free(heap, cabs, ncabs);
                    rp_mem_free(heap, msi);
                    rp_srcdiag_print(&d, src);
                    rc = src_rc;
                    goto done;
                }
                rp_mem_free(heap, msi);
                msi = signed_msi;
                msi_len = signed_len;
            }
            // RFC-0001 7.1: never overwrite part of an earlier multi-file output; cabinets first, the
            // package last, so a package on disk always has its cabinets.
            for (size_t i = 0; !lint && err == PROVEN_OK && i < ncabs; ++i) {
                snprintf(cabpath, sizeof cabpath, "%.*s%s", (int)outdir, out, cabs[i].name);
                uint64_t size = 0;
                if (rp_pal_stat(heap, cabpath, &size) != RP_FS_NONE) {
                    rp_diag_error(RP_DIAG_OUTPUT, "'%s' already exists; remove it or build into another folder", cabpath);
                    err = PROVEN_ERR_IO;
                }
            }
            for (size_t i = 0; !lint && err == PROVEN_OK && i < ncabs; ++i) {
                snprintf(cabpath, sizeof cabpath, "%.*s%s", (int)outdir, out, cabs[i].name);
                err = rp_pal_write_file_atomic(heap, cabpath, cabs[i].data, cabs[i].len);
                if (err != PROVEN_OK) rp_diag_error(RP_DIAG_OUTPUT, "cannot write '%s'", cabpath);
            }
            if (!lint && err == PROVEN_OK) {
                err = rp_pal_write_file_atomic(heap, out, msi, msi_len);
                if (err != PROVEN_OK) rp_diag_error(RP_DIAG_OUTPUT, "cannot write '%s'", out);
            }
            rp_build_files_free(heap, cabs, ncabs);
            rp_mem_free(heap, msi);
            rp_srcdiag_print(&d, src);
            rc = err == PROVEN_OK                  ? RP_EXIT_OK
                 : err == PROVEN_ERR_INVALID_STATE ? RP_EXIT_LINT     // rp_msi_lint refused the tables
                 : d.errors                        ? RP_EXIT_SOURCE
                                                   : RP_EXIT_IO;
            if (rc == RP_EXIT_OK && strict && d.warnings) rc = RP_EXIT_LINT;
        } else {
            rp_srcdiag_print(&d, src);
            rc = err == PROVEN_ERR_INVALID_FORMAT ? RP_EXIT_SOURCE : RP_EXIT_IO;
        }
    }
done:
    for (size_t k = 0; k < ndef; ++k) rp_mem_free(proven_heap_allocator(), define_buf[k]);
    return rc;
}

int rp_cmd_build(int argc, char **argv) { return run(argc, argv, false); }

int rp_cmd_lint_source(int argc, char **argv) { return run(argc, argv, true); }
