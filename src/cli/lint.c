// src/cli/lint.c - `rubrapack lint <src.rpk|file.msi> [--strict]` (RFC-0006 1). A source goes
// through the build steps without writing (build.c); a package made by any tool is read and
// checked with the same table rules (rubrapack/lint.h, foreign mode).

#include "rubrapack/cfb.h"
#include "rubrapack/diag.h"
#include "rubrapack/inspect.h"
#include "rubrapack/lint.h"
#include "rubrapack/mem.h"
#include "rubrapack/msi.h"
#include "rubrapack/msix.h"
#include "rubrapack/pal.h"

#include <stdio.h>
#include <string.h>

#include "proven/heap.h"

enum { MAX_INPUT = 1u << 30 };

static bool ends_with_ci(const char *s, const char *suffix) {
    size_t n = strlen(s), m = strlen(suffix);
    if (n < m) return false;
    for (size_t k = 0; k < m; ++k) {
        char a = s[n - m + k];
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (a != suffix[k]) return false;
    }
    return true;
}

static int lint_package(const char *path, bool strict) {
    static const uint16_t summary_name[] = { 5, 'S', 'u', 'm', 'm', 'a', 'r', 'y', 'I', 'n', 'f', 'o', 'r', 'm', 'a', 't', 'i', 'o', 'n' };
    proven_allocator_t heap = proven_heap_allocator();
    rp_limits_t limits = rp_limits_default();
    uint8_t *data = NULL, *sum = NULL;
    size_t len = 0, sum_len = 0;
    proven_err_t err = rp_pal_read_file(heap, path, MAX_INPUT, &data, &len);
    if (err != PROVEN_OK) {
        rp_diag_error(RP_DIAG_INPUT, "cannot read '%s' (%s)", path, err == PROVEN_ERR_NOT_FOUND ? "not found" : "read error");
        return RP_EXIT_IO;
    }
    rp_cfb_t cfb;
    rp_msi_t msi;
    const char *why = NULL;
    if (rp_cfb_open(&cfb, heap, data, len, &limits, &why) != PROVEN_OK) {
        rp_diag_error(RP_DIAG_BAD_PACKAGE, "'%s' is not a valid package: %s", path, why ? why : "unreadable");
        rp_mem_free(heap, data);
        return RP_EXIT_IO;
    }
    if (rp_msi_open(&msi, heap, &cfb, &limits, &why) != PROVEN_OK) {
        rp_diag_error(RP_DIAG_BAD_PACKAGE, "'%s' is not a valid MSI database: %s", path, why ? why : "unreadable");
        rp_cfb_close(&cfb);
        rp_mem_free(heap, data);
        return RP_EXIT_IO;
    }
    uint32_t id;
    if (rp_cfb_find(&cfb, 0, summary_name, sizeof summary_name / sizeof summary_name[0], &id) == PROVEN_OK &&
        cfb.entries[id].size <= limits.max_metadata) {
        sum_len = (size_t)cfb.entries[id].size;
        sum = rp_mem_alloc(heap, sum_len + 1, 1);
        if (sum == NULL || rp_cfb_read(&cfb, id, sum, sum_len) != PROVEN_OK) sum_len = 0;
    }
    int rc;
    rp_msi_view_t view;
    rp_srcdiags_t d = { 0 };
    err = rp_msi_view(&msi, sum_len ? sum : NULL, sum_len, &view);
    if (err != PROVEN_OK) {
        rp_diag_error(RP_DIAG_BAD_PACKAGE, "'%s': a table cannot be read", path);
        rc = RP_EXIT_IO;
    } else {
        rp_lint_opts_t opts = { .foreign = true, .strict = strict };
        err = rp_msi_lint_opts(heap, &view.db, &opts, &d);
        rp_msi_view_free(&msi, &view);
        rp_srcdiag_print(&d, path);
        char line[512];
        snprintf(line, sizeof line, "%s: %zu error%s, %zu warning%s\n", path, d.errors, d.errors == 1 ? "" : "s", d.warnings,
                 d.warnings == 1 ? "" : "s");
        rc = err == PROVEN_ERR_NOMEM ? RP_EXIT_IO : d.errors ? RP_EXIT_LINT : RP_EXIT_OK;
        if (rp_pal_puts(RP_OUT_STDOUT, line) != PROVEN_OK && rc == RP_EXIT_OK) rc = RP_EXIT_IO;
    }
    rp_mem_free(heap, sum);
    rp_msi_close(&msi);
    rp_cfb_close(&cfb);
    rp_mem_free(heap, data);
    return rc;
}

int rp_cmd_lint(int argc, char **argv) {
    const char *target = NULL;
    bool strict = false, other = false;
    for (int i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--strict") == 0) strict = true;
        else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            other = true;
            if ((strcmp(argv[i], "-D") == 0 || strcmp(argv[i], "--arch") == 0) && i + 1 < argc) ++i;
        } else if (target == NULL) target = argv[i];
    }
    if (target && (ends_with_ci(target, ".msix") || ends_with_ci(target, ".msixbundle"))) {
        if (other || argc > 4 || (argc == 4 && !strict)) {
            rp_diag_error(RP_DIAG_EXTRA_ARGUMENT, "usage: rubrapack lint <file.msix|file.msixbundle> [--strict]");
            return RP_EXIT_USAGE;
        }
        return rp_msix_lint(target, strict);
    }
    if (target && ends_with_ci(target, ".msi")) {
        if (other || argc > 4 || (argc == 4 && !strict)) {
            rp_diag_error(RP_DIAG_EXTRA_ARGUMENT, "usage: rubrapack lint <file.msi> [--strict]");
            return RP_EXIT_USAGE;
        }
        return lint_package(target, strict);
    }
    return rp_cmd_lint_source(argc, argv);
}
