// src/cli/container.c - `rubrapack container <name> <package|family>... -o <out.xml>` (RFC-0031 B21): a
// shared package container's definition, the file Add-AppSharedPackageContainer deploys. Each
// package (a .msix or .msixbundle) gives its family name, Identity Name and the publisher ID; a
// family name may also be given as it is. The first one has the highest priority.

#include "rubrapack/cli.h"
#include "rubrapack/diag.h"
#include "rubrapack/inspect.h"
#include "rubrapack/limits.h"
#include "rubrapack/mem.h"
#include "rubrapack/msix.h"
#include "rubrapack/pal.h"
#include "rubrapack/str.h"

#include <stdio.h>
#include <string.h>

#include "proven/heap.h"

enum { MAX_FAMILIES = 64 };

static void xml_escaped(rp_buf_t *b, const char *s) {
    for (; *s; ++s) {
        if (*s == '&') rp_buf_puts(b, "&amp;");
        else if (*s == '<') rp_buf_puts(b, "&lt;");
        else if (*s == '>') rp_buf_puts(b, "&gt;");
        else if (*s == '"') rp_buf_puts(b, "&quot;");
        else rp_buf_byte(b, (uint8_t)*s);
    }
}

// "<Name>_<13 letters and digits>", as Windows writes a package family name.
bool rp_family_name_ok(const char *s) {
    const char *u = strrchr(s, '_');
    if (u == NULL || u == s || strlen(u + 1) != 13) return false;
    for (const char *p = s; p < u; ++p) {
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '.' || *p == '-')) return false;
    }
    for (const char *p = u + 1; *p; ++p) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9'))) return false;
    }
    return true;
}

static int family_of(proven_allocator_t heap, const char *path, char *out, size_t cap) {
    rp_limits_t lim = rp_limits_default();
    uint8_t *data = NULL, *man = NULL;
    size_t len = 0, nf = 0, ml = 0;
    rp_msix_file_t *files = NULL;
    const char *why = NULL;
    if (rp_pal_read_file(heap, path, (size_t)lim.max_output, &data, &len) != PROVEN_OK) {
        rp_diag_error(RP_DIAG_INPUT, "cannot read '%s'", path);
        return RP_EXIT_IO;
    }
    int rc = RP_EXIT_OK;
    char name[256] = "", publisher[8400] = "";
    if (rp_msix_open(heap, data, len, &lim, &files, &nf, &man, &ml, &why) != PROVEN_OK ||
        !rp_xml_attr((const char *)man, ml, "Identity", "Name", name, sizeof name) ||
        !rp_xml_attr((const char *)man, ml, "Identity", "Publisher", publisher, sizeof publisher)) {
        rp_diag_error(RP_DIAG_BAD_PACKAGE, "'%s' is not a valid MSIX package: %s", path, why ? why : "unreadable");
        rc = RP_EXIT_IO;
    } else {
        char id[14];
        rp_msix_publisher_id(publisher, id);
        snprintf(out, cap, "%s_%s", name, id);
    }
    rp_msix_files_free(heap, files, nf);
    rp_mem_free(heap, man);
    rp_mem_free(heap, data);
    return rc;
}

int rp_cmd_container(int argc, char **argv) {
    const char *name = NULL, *out = NULL, *in[MAX_FAMILIES];
    size_t nin = 0;
    for (int i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            out = argv[++i];
        } else if (argv[i][0] == '-') {
            (void)rp_cli_usage_error("container");
            return RP_EXIT_USAGE;
        } else if (name == NULL) {
            name = argv[i];
        } else if (nin < MAX_FAMILIES) {
            in[nin++] = argv[i];
        } else {
            rp_diag_error(RP_DIAG_EXTRA_ARGUMENT, "a container holds at most %u package families", (unsigned)MAX_FAMILIES);
            return RP_EXIT_USAGE;
        }
    }
    if (name == NULL || nin == 0 || out == NULL || name[0] == 0 || strlen(name) > 255) {
        (void)rp_cli_usage_error("container");
        return RP_EXIT_USAGE;
    }
    proven_allocator_t heap = proven_heap_allocator();
    static char families[MAX_FAMILIES][320];
    for (size_t k = 0; k < nin; ++k) {
        if (rp_ascii_ends_with_ci(in[k], ".msix") || rp_ascii_ends_with_ci(in[k], ".msixbundle")) {
            int rc = family_of(heap, in[k], families[k], sizeof families[k]);
            if (rc != RP_EXIT_OK) return rc;
        } else if (rp_family_name_ok(in[k])) {
            snprintf(families[k], sizeof families[k], "%s", in[k]);
        } else {
            rp_diag_error(RP_DIAG_EXTRA_ARGUMENT, "'%s' is neither an MSIX package nor a package family name (<Name>_<publisher ID>)", in[k]);
            return RP_EXIT_USAGE;
        }
        for (size_t j = 0; j < k; ++j) {
            if (rp_ascii_ieq(families[j], families[k])) {
                rp_diag_error(RP_DIAG_EXTRA_ARGUMENT, "the package family %s is in the container twice", families[k]);
                return RP_EXIT_USAGE;
            }
        }
    }
    rp_buf_t b = rp_buf_new(heap, 1u << 20);
    rp_buf_puts(&b, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n<AppSharedPackageContainer Name=\"");
    xml_escaped(&b, name);
    rp_buf_puts(&b, "\">\r\n");
    for (size_t k = 0; k < nin; ++k) {
        rp_buf_puts(&b, "  <PackageFamily Name=\"");
        xml_escaped(&b, families[k]);
        rp_buf_puts(&b, "\"/>\r\n");
    }
    rp_buf_puts(&b, "</AppSharedPackageContainer>\r\n");
    int rc = RP_EXIT_OK;
    if (b.err != PROVEN_OK || rp_pal_write_file_atomic(heap, out, b.data, b.len) != PROVEN_OK) {
        rp_diag_error(RP_DIAG_OUTPUT, "cannot write '%s'", out);
        rc = RP_EXIT_IO;
    }
    rp_buf_free(&b);
    return rc;
}
