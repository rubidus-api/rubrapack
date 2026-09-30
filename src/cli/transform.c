// src/cli/transform.c - `rubrapack transform <base.msi> <target.msi> -o <out.mst>` and
// `rubrapack inspect <file.mst> --base <base.msi> | --summary` (RFC-0016 3, rubrapack/transform.h).

#include "rubrapack/diag.h"
#include "rubrapack/inspect.h"
#include "rubrapack/mem.h"
#include "rubrapack/pal.h"
#include "rubrapack/suminfo.h"
#include "rubrapack/transform.h"

#include <stdio.h>
#include <string.h>

#include "proven/heap.h"

enum { MAX_INPUT = 1u << 30 };

static const char usage[] = "usage: rubrapack transform <base.msi> <target.msi> -o <out.mst> [--validate none|<list>]";

// --validate: "none", or names joined by commas.
static bool validate_flags(const char *s, uint32_t *flags) {
    *flags = 0;
    if (strcmp(s, "none") == 0) return true;
    static const struct { const char *name; uint32_t bit; } names[] = {
        { "product-code", RP_MST_VALIDATE_PRODUCT }, { "upgrade-code", RP_MST_VALIDATE_UPGRADE_CODE },
        { "language", RP_MST_VALIDATE_LANGUAGE }, { "platform", RP_MST_VALIDATE_PLATFORM },
    };
    while (*s) {
        const char *e = strchr(s, ',');
        size_t n = e ? (size_t)(e - s) : strlen(s);
        bool found = false;
        for (size_t i = 0; i < sizeof names / sizeof names[0]; ++i) {
            if (strlen(names[i].name) == n && memcmp(names[i].name, s, n) == 0) {
                *flags |= names[i].bit;
                found = true;
            }
        }
        if (!found) return false;
        s += n + (e ? 1 : 0);
        if (e && *s == '\0') return false;
    }
    return *flags != 0;
}

int rp_cmd_transform(int argc, char **argv) {
    const char *in[2] = { NULL, NULL }, *out = NULL;
    uint32_t flags = RP_MST_VALIDATE_PRODUCT | RP_MST_VALIDATE_UPGRADE_CODE;
    size_t nin = 0;
    for (int i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            out = argv[++i];
        } else if (strcmp(argv[i], "--validate") == 0 && i + 1 < argc) {
            if (!validate_flags(argv[++i], &flags)) {
                rp_diag_error(RP_DIAG_EXTRA_ARGUMENT, "--validate takes none, or some of product-code,upgrade-code,language,platform");
                return RP_EXIT_USAGE;
            }
        } else if (argv[i][0] != '-' && nin < 2) {
            in[nin++] = argv[i];
        } else {
            rp_diag_error(RP_DIAG_EXTRA_ARGUMENT, "%s", usage);
            return RP_EXIT_USAGE;
        }
    }
    if (nin != 2 || out == NULL) {
        rp_diag_error(RP_DIAG_EXTRA_ARGUMENT, "%s", usage);
        return RP_EXIT_USAGE;
    }
    proven_allocator_t heap = proven_heap_allocator();
    rp_limits_t limits = rp_limits_default();
    rp_pkg_t a, b;
    int rc = rp_pkg_open(heap, in[0], &limits, &a);
    if (rc == RP_EXIT_OK) rc = rp_pkg_open(heap, in[1], &limits, &b);
    else b = (rp_pkg_t){ 0 };
    if (rc == RP_EXIT_OK) {
        rp_mst_side_t sa = { &a.view.db, &a.cfb }, sb = { &b.view.db, &b.cfb };
        uint8_t *mst = NULL;
        size_t len = 0;
        const char *why = NULL;
        char table[64];
        proven_err_t err = rp_mst_write(heap, &sa, &sb, flags, &limits, &mst, &len, &why, table, sizeof table);
        if (err == PROVEN_ERR_UNSUPPORTED && why) {
            if (table[0]) rp_diag_error(RP_DIAG_TRANSFORM, "table %s %s", table, why);
            else rp_diag_error(RP_DIAG_TRANSFORM, "%s", why);
            rc = RP_EXIT_SOURCE;
        } else if (err != PROVEN_OK) {
            rp_diag_error(err == PROVEN_ERR_NOMEM ? RP_DIAG_NOMEM : RP_DIAG_BAD_PACKAGE, "cannot make a transform from '%s' to '%s'", in[0], in[1]);
            rc = RP_EXIT_IO;
        } else if (rp_pal_write_file_atomic(heap, out, mst, len) != PROVEN_OK) {
            rp_diag_error(RP_DIAG_OUTPUT, "cannot write '%s'", out);
            rc = RP_EXIT_IO;
        }
        rp_mem_free(heap, mst);
    }
    rp_pkg_close(heap, &a);
    rp_pkg_close(heap, &b);
    return rc;
}

int rp_mst_inspect(const char *path, const char *what, const char *base) {
    static const uint16_t summary_name[] = { 5, 'S', 'u', 'm', 'm', 'a', 'r', 'y', 'I', 'n', 'f', 'o', 'r', 'm', 'a', 't', 'i', 'o', 'n' };
    if (!((base && what == NULL) || (base == NULL && what && strcmp(what, "--summary") == 0))) {
        rp_diag_error(RP_DIAG_EXTRA_ARGUMENT, "usage: rubrapack inspect <file.mst> --base <base.msi> | --summary | --streams");
        return RP_EXIT_USAGE;
    }
    proven_allocator_t heap = proven_heap_allocator();
    rp_limits_t limits = rp_limits_default();
    uint8_t *data = NULL;
    size_t len = 0;
    if (rp_pal_read_file(heap, path, MAX_INPUT, &data, &len) != PROVEN_OK) {
        rp_diag_error(RP_DIAG_INPUT, "cannot read '%s'", path);
        return RP_EXIT_IO;
    }
    rp_cfb_t cfb;
    const char *why = NULL;
    if (rp_cfb_open(&cfb, heap, data, len, &limits, &why) != PROVEN_OK) {
        rp_diag_error(RP_DIAG_BAD_PACKAGE, "'%s' is not a valid transform: %s", path, why ? why : "unreadable");
        rp_mem_free(heap, data);
        return RP_EXIT_IO;
    }
    int rc = RP_EXIT_OK;
    rp_buf_t out = rp_buf_new(heap, limits.max_metadata);
    if (base == NULL) {
        uint32_t id;
        rp_suminfo_t si = { .count = 0 };
        uint8_t *sum = NULL, *idt = NULL;
        size_t n = 0, idt_len = 0;
        if (rp_cfb_find(&cfb, 0, summary_name, sizeof summary_name / sizeof summary_name[0], &id) == PROVEN_OK) {
            n = (size_t)cfb.entries[id].size;
            sum = rp_mem_alloc(heap, n + 1, 1);
        }
        if (sum == NULL || rp_cfb_read(&cfb, id, sum, n) != PROVEN_OK || rp_suminfo_parse(sum, n, &si) != PROVEN_OK ||
            rp_suminfo_export_idt(&si, heap, &idt, &idt_len) != PROVEN_OK) {
            rp_diag_error(RP_DIAG_BAD_PACKAGE, "'%s' has no readable summary information", path);
            rc = RP_EXIT_IO;
        } else {
            rp_buf_put(&out, idt, idt_len);
        }
        rp_mem_free(heap, idt);
        rp_mem_free(heap, sum);
    } else {
        rp_pkg_t b;
        rc = rp_pkg_open(heap, base, &limits, &b);
        if (rc == RP_EXIT_OK && rp_mst_list(heap, &cfb, &b.view.db, &limits, &out, &why) != PROVEN_OK) {
            rp_diag_error(RP_DIAG_BAD_PACKAGE, "'%s' cannot be read against '%s': %s", path, base, why && *why ? why : "unreadable");
            rc = RP_EXIT_IO;
        }
        rp_pkg_close(heap, &b);
    }
    if (rc == RP_EXIT_OK && (out.err != PROVEN_OK || rp_pal_write(RP_OUT_STDOUT, out.data, out.len) != PROVEN_OK)) rc = RP_EXIT_IO;
    rp_buf_free(&out);
    rp_cfb_close(&cfb);
    rp_mem_free(heap, data);
    return rc;
}
