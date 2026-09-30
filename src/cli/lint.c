// src/cli/lint.c - `rubrapack lint <src.toml|file.msi> [--strict]` (RFC-0006 1). A source goes
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

void rp_pkg_close(proven_allocator_t heap, rp_pkg_t *p) {
    if (p->stage >= 4) rp_msi_view_free(&p->msi, &p->view);
    rp_mem_free(heap, p->sum);
    if (p->stage >= 3) rp_msi_close(&p->msi);
    if (p->stage >= 2) rp_cfb_close(&p->cfb);
    rp_mem_free(heap, p->data);
}

int rp_pkg_open(proven_allocator_t heap, const char *path, const rp_limits_t *limits, rp_pkg_t *p) {
    static const uint16_t summary_name[] = { 5, 'S', 'u', 'm', 'm', 'a', 'r', 'y', 'I', 'n', 'f', 'o', 'r', 'm', 'a', 't', 'i', 'o', 'n' };
    *p = (rp_pkg_t){ 0 };
    size_t len = 0;
    proven_err_t err = rp_pal_read_file(heap, path, MAX_INPUT, &p->data, &len);
    if (err != PROVEN_OK) {
        rp_diag_error(RP_DIAG_INPUT, "cannot read '%s' (%s)", path, err == PROVEN_ERR_NOT_FOUND ? "not found" : "read error");
        return RP_EXIT_IO;
    }
    p->stage = 1;
    const char *why = NULL;
    if (rp_cfb_open(&p->cfb, heap, p->data, len, limits, &why) != PROVEN_OK) {
        rp_diag_error(RP_DIAG_BAD_PACKAGE, "'%s' is not a valid package: %s", path, why ? why : "unreadable");
        return RP_EXIT_IO;
    }
    p->stage = 2;
    if (rp_msi_open(&p->msi, heap, &p->cfb, limits, &why) != PROVEN_OK) {
        rp_diag_error(RP_DIAG_BAD_PACKAGE, "'%s' is not a valid MSI database: %s", path, why ? why : "unreadable");
        return RP_EXIT_IO;
    }
    p->stage = 3;
    uint32_t id;
    size_t sum_len = 0;
    if (rp_cfb_find(&p->cfb, 0, summary_name, sizeof summary_name / sizeof summary_name[0], &id) == PROVEN_OK &&
        p->cfb.entries[id].size <= limits->max_metadata) {
        sum_len = (size_t)p->cfb.entries[id].size;
        p->sum = rp_mem_alloc(heap, sum_len + 1, 1);
        if (p->sum == NULL || rp_cfb_read(&p->cfb, id, p->sum, sum_len) != PROVEN_OK) sum_len = 0;
    }
    if (rp_msi_view(&p->msi, sum_len ? p->sum : NULL, sum_len, &p->view) != PROVEN_OK) {
        rp_diag_error(RP_DIAG_BAD_PACKAGE, "'%s': a table cannot be read", path);
        return RP_EXIT_IO;
    }
    p->stage = 4;
    return RP_EXIT_OK;
}

// `lint new.msi --previous old.msi` (RFC-0013 R3): the package's own rules, then the upgrade from
// the previous version.
static int lint_against(const char *path, const char *previous, bool strict) {
    proven_allocator_t heap = proven_heap_allocator();
    rp_limits_t limits = rp_limits_default();
    rp_pkg_t a, b;
    int rc = rp_pkg_open(heap, path, &limits, &a);
    if (rc == RP_EXIT_OK) rc = rp_pkg_open(heap, previous, &limits, &b);
    else b = (rp_pkg_t){ 0 };
    if (rc == RP_EXIT_OK) {
        rp_srcdiags_t d = { 0 };
        rp_lint_opts_t opts = { .foreign = true, .strict = strict };
        proven_err_t err = rp_msi_lint_opts(heap, &a.view.db, &opts, &d);
        if (err == PROVEN_OK) err = rp_msi_lint_previous(heap, &a.view.db, &b.view.db, &d);
        rp_srcdiag_print(&d, path);
        char line[512];
        snprintf(line, sizeof line, "%s: %zu error%s, %zu warning%s (against %s)\n", path, d.errors, d.errors == 1 ? "" : "s",
                 d.warnings, d.warnings == 1 ? "" : "s", previous);
        rc = err == PROVEN_ERR_NOMEM ? RP_EXIT_IO : d.errors || (strict && d.warnings) ? RP_EXIT_LINT : RP_EXIT_OK;
        if (rp_pal_puts(RP_OUT_STDOUT, line) != PROVEN_OK && rc == RP_EXIT_OK) rc = RP_EXIT_IO;
    }
    rp_pkg_close(heap, &a);
    rp_pkg_close(heap, &b);
    return rc;
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
        rc = err == PROVEN_ERR_NOMEM ? RP_EXIT_IO : d.errors || (strict && d.warnings) ? RP_EXIT_LINT : RP_EXIT_OK;
        if (rp_pal_puts(RP_OUT_STDOUT, line) != PROVEN_OK && rc == RP_EXIT_OK) rc = RP_EXIT_IO;
    }
    rp_mem_free(heap, sum);
    rp_msi_close(&msi);
    rp_cfb_close(&cfb);
    rp_mem_free(heap, data);
    return rc;
}

int rp_cmd_lint(int argc, char **argv) {
    const char *target = NULL, *previous = NULL;
    bool strict = false, other = false;
    for (int i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--strict") == 0) strict = true;
        else if (strcmp(argv[i], "--previous") == 0 && i + 1 < argc) previous = argv[++i];
        else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            other = true;
            if ((strcmp(argv[i], "-D") == 0 || strcmp(argv[i], "--arch") == 0 || strcmp(argv[i], "--target") == 0) && i + 1 < argc) ++i;
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
        int extra = argc - 3 - (strict ? 1 : 0) - (previous ? 2 : 0);
        if (other || extra != 0 || (previous && !ends_with_ci(previous, ".msi"))) {
            rp_diag_error(RP_DIAG_EXTRA_ARGUMENT, "usage: rubrapack lint <file.msi> [--previous <old.msi>] [--strict]");
            return RP_EXIT_USAGE;
        }
        return previous ? lint_against(target, previous, strict) : lint_package(target, strict);
    }
    if (previous) {
        rp_diag_error(RP_DIAG_EXTRA_ARGUMENT, "--previous compares two .msi packages");
        return RP_EXIT_USAGE;
    }
    return rp_cmd_lint_source(argc, argv);
}
