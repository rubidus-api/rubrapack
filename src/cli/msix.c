// src/cli/msix.c - `rubrapack inspect` and `rubrapack lint` for MSIX packages and bundles (RFC-0009 1,
// RFC-0010 P8b-2).

#include "rubrapack/diag.h"
#include "rubrapack/inspect.h"
#include "rubrapack/mem.h"
#include "rubrapack/msix.h"
#include "rubrapack/pal.h"

#include <stdio.h>
#include <string.h>

#include "proven/heap.h"

enum { MAX_INPUT = 1u << 30 };

typedef struct {
    uint8_t        *data, *manifest;
    size_t          len, manifest_len, count;
    rp_msix_file_t *files;
} pkg_t;

static int open_pkg(const char *path, pkg_t *p) {
    proven_allocator_t heap = proven_heap_allocator();
    rp_limits_t lim = rp_limits_default();
    memset(p, 0, sizeof *p);
    if (rp_pal_read_file(heap, path, MAX_INPUT, &p->data, &p->len) != PROVEN_OK) {
        rp_diag_error(RP_DIAG_INPUT, "cannot read '%s'", path);
        return RP_EXIT_IO;
    }
    const char *why = NULL;
    proven_err_t err = rp_msix_open(heap, p->data, p->len, &lim, &p->files, &p->count, &p->manifest, &p->manifest_len, &why);
    if (err != PROVEN_OK) {
        rp_diag_error(RP_DIAG_BAD_PACKAGE, "'%s' is not a valid MSIX package: %s", path, why ? why : "unreadable");
        rp_mem_free(heap, p->data);
        return err == PROVEN_ERR_NOMEM ? RP_EXIT_IO : RP_EXIT_LINT;
    }
    return RP_EXIT_OK;
}

static void close_pkg(pkg_t *p) {
    proven_allocator_t heap = proven_heap_allocator();
    rp_msix_files_free(heap, p->files, p->count);
    rp_mem_free(heap, p->manifest);
    rp_mem_free(heap, p->data);
}

static int out(const char *s) { return rp_pal_puts(RP_OUT_STDOUT, s) == PROVEN_OK ? RP_EXIT_OK : RP_EXIT_IO; }

int rp_msix_inspect(const char *path, const char *what) {
    if (what && strcmp(what, "--files") != 0 && strcmp(what, "--manifest") != 0) {
        rp_diag_error(RP_DIAG_EXTRA_ARGUMENT, "usage: rubrapack inspect <file.msix|file.msixbundle> [--files | --manifest]");
        return RP_EXIT_USAGE;
    }
    pkg_t p;
    int rc = open_pkg(path, &p);
    if (rc != RP_EXIT_OK) return rc == RP_EXIT_LINT ? RP_EXIT_IO : rc;
    char line[5000];
    if (what && strcmp(what, "--manifest") == 0) {
        rc = out((const char *)p.manifest);
    } else if (what) {
        for (size_t i = 0; i < p.count && rc == RP_EXIT_OK; ++i) {
            snprintf(line, sizeof line, "%s\t%llu\t%s\n", p.files[i].name, (unsigned long long)p.files[i].size, p.files[i].deflated ? "deflate" : "stored");
            rc = out(line);
        }
    } else {
        static const char *const fields[][2] = { { "Identity", "Name" }, { "Identity", "Publisher" }, { "Identity", "Version" },
                                                 { "Identity", "ProcessorArchitecture" }, { "Application", "Executable" },
                                                 { "TargetDeviceFamily", "MinVersion" } };
        uint64_t total = 0;
        for (size_t i = 0; i < p.count; ++i) total += p.files[i].size;
        bool bundle = strstr((const char *)p.manifest, "<Bundle") != NULL;
        for (size_t i = 0; i < (bundle ? 3 : sizeof fields / sizeof fields[0]) && rc == RP_EXIT_OK; ++i) {
            char v[4200];
            if (!rp_xml_attr((const char *)p.manifest, p.manifest_len, fields[i][0], fields[i][1], v, sizeof v)) snprintf(v, sizeof v, "-");
            snprintf(line, sizeof line, "%s: %s\n", fields[i][1], v);
            rc = out(line);
        }
        if (bundle) {               // its packages (after the manifest), each opened and checked
            for (size_t i = 1; i < p.count && rc == RP_EXIT_OK; ++i) {
                snprintf(line, sizeof line, "package: %s (%llu bytes)\n", p.files[i].name, (unsigned long long)p.files[i].size);
                rc = out(line);
            }
        } else {
            snprintf(line, sizeof line, "files: %zu (%llu bytes, block hashes checked)\n", p.count, (unsigned long long)total);
            if (rc == RP_EXIT_OK) rc = out(line);
        }
    }
    close_pkg(&p);
    return rc;
}

// The package's structure and block hashes (rp_msix_open), then what an install needs from the
// manifest: identity, and the files it names.
int rp_msix_lint(const char *path, bool strict) {
    (void)strict;                   // no warnings yet
    pkg_t p;
    int rc = open_pkg(path, &p);
    if (rc != RP_EXIT_OK) return rc;
    size_t errors = 0;
    char v[4200], line[4400];
    static const char *const need[][2] = { { "Identity", "Name" }, { "Identity", "Publisher" }, { "Identity", "Version" } };
    for (size_t i = 0; i < 3; ++i) {
        if (!rp_xml_attr((const char *)p.manifest, p.manifest_len, need[i][0], need[i][1], v, sizeof v) || v[0] == 0) {
            rp_diag_error("RP2201", "'%s': the manifest has no %s %s", path, need[i][0], need[i][1]);
            ++errors;
        }
    }
    // Files the manifest points at must be in the package.
    static const char *const refs[][2] = { { "Application", "Executable" }, { "uap:VisualElements", "Square150x150Logo" },
                                           { "uap:VisualElements", "Square44x44Logo" }, { "Logo", NULL } };
    for (size_t i = 0; i < sizeof refs / sizeof refs[0]; ++i) {
        bool have;
        if (refs[i][1]) {
            have = rp_xml_attr((const char *)p.manifest, p.manifest_len, refs[i][0], refs[i][1], v, sizeof v);
        } else {                    // <Logo>text</Logo>
            const char *s = strstr((const char *)p.manifest, "<Logo>"), *e = s ? strstr(s, "</Logo>") : NULL;
            have = s && e && (size_t)(e - s - 6) < sizeof v;
            if (have) snprintf(v, sizeof v, "%.*s", (int)(e - s - 6), s + 6);
        }
        if (!have) continue;
        bool found = false;
        for (size_t k = 0; k < p.count && !found; ++k) found = strcmp(p.files[k].name, v) == 0;
        if (!found) {
            rp_diag_error("RP2202", "'%s': the manifest names '%s', which is not in the package", path, v);
            ++errors;
        }
    }
    snprintf(line, sizeof line, "%s: %zu files, %zu errors, 0 warnings\n", path, p.count, errors);
    rc = out(line);
    close_pkg(&p);
    return errors ? RP_EXIT_LINT : rc;
}
