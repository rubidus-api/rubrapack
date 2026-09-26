// src/cli/build.c - `rubrapack build <src.rpk> -o <out.msi> [options]` (RFC-0001 7, 7.1).

#include "rubrapack/build.h"
#include "rubrapack/diag.h"
#include "rubrapack/inspect.h"
#include "rubrapack/ir.h"
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

int rp_cmd_build(int argc, char **argv) {
    const char *src = NULL, *out = NULL, *arch = NULL, *compress = NULL;
    rp_define_t defines[MAX_DEFINES];
    char *define_buf[MAX_DEFINES];
    size_t ndef = 0;
    bool reproducible = false;
    int rc = RP_EXIT_USAGE;

    for (int i = 2; i < argc; ++i) {
        const char *a = argv[i];
        const char *next = i + 1 < argc ? argv[i + 1] : NULL;
        if (strcmp(a, "-o") == 0 && next) {
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
        } else if (strcmp(a, "--reproducible") == 0) {
            reproducible = true;
        } else if (strcmp(a, "--target") == 0 && next) {
            if (strcmp(next, "msi") != 0) {
                rp_diag_error(RP_DIAG_NOT_IMPLEMENTED, "--target %s is not implemented yet (MSIX is planned for P8)", next);
                goto done;
            }
            ++i;
        } else if (a[0] == '-' && a[1] != '\0') {
            rp_diag_error(RP_DIAG_EXTRA_ARGUMENT, "unknown or incomplete build option '%s'", a);
            goto done;
        } else if (src == NULL) {
            src = a;
        } else {
            rp_diag_error(RP_DIAG_EXTRA_ARGUMENT, "more than one source file ('%s')", a);
            goto done;
        }
    }
    if (src == NULL || out == NULL) {
        rp_diag_error(RP_DIAG_EXTRA_ARGUMENT, "usage: rubrapack build <src.rpk> -o <out.msi> [-D NAME=VALUE] [--arch x64|arm64|x86] [--compress none] [--reproducible]");
        goto done;
    }
    if (!ends_with(out, ".msi")) {
        rp_diag_error(RP_DIAG_NOT_IMPLEMENTED, "output '%s': only .msi is implemented yet (.msix is planned for P8)", out);
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
        if (err == PROVEN_OK) {
            rp_ir_options_t opt = { dir, defines, ndef, arch, compress, out };
            err = rp_ir_build(heap, &doc, &opt, &ir, &d);
            rp_toml_free(&doc);
        }
        rp_mem_free(heap, text);
        if (err == PROVEN_OK) {
            rp_limits_t limits = rp_limits_default();
            rp_build_options_t bopt = { reproducible };
            uint8_t *msi = NULL;
            size_t msi_len = 0;
            err = rp_msi_from_ir(heap, &ir, &bopt, &limits, &msi, &msi_len, &d);
            rp_ir_free(&ir);
            if (err == PROVEN_OK) {
                err = rp_pal_write_file_atomic(heap, out, msi, msi_len);
                if (err != PROVEN_OK) rp_diag_error(RP_DIAG_OUTPUT, "cannot write '%s'", out);
            }
            rp_mem_free(heap, msi);
            rp_srcdiag_print(&d, src);
            rc = err == PROVEN_OK ? RP_EXIT_OK : (d.errors ? RP_EXIT_SOURCE : RP_EXIT_IO);
        } else {
            rp_srcdiag_print(&d, src);
            rc = err == PROVEN_ERR_INVALID_FORMAT ? RP_EXIT_SOURCE : RP_EXIT_IO;
        }
    }
done:
    for (size_t k = 0; k < ndef; ++k) rp_mem_free(proven_heap_allocator(), define_buf[k]);
    return rc;
}
