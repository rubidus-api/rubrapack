// src/cli/transform.c - `rubrapack transform <base.msi> <target.msi> -o <out.mst>` and
// `rubrapack inspect <file.mst> --base <base.msi> | --summary` (RFC-0016 3, rubrapack/transform.h).

#include "rubrapack/diag.h"
#include "rubrapack/inspect.h"
#include "rubrapack/mem.h"
#include "rubrapack/pal.h"
#include "rubrapack/patch.h"
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
        rp_mst_opts_t opts = { .summary_flags = flags << 16 };      // checks in the high word, as msi.dll stores them
        proven_err_t err = rp_mst_write(heap, &sa, &sb, &opts, &limits, &mst, &len, &why, table, sizeof table);
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
        if (rc == RP_EXIT_OK && rp_mst_list(heap, &cfb, 0, &b.view.db, &limits, &out, &why) != PROVEN_OK) {
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

// `rubrapack patch <base.msi> <target.msi> -o <out.msp>` (RFC-0016 3, rubrapack/patch.h).
int rp_cmd_patch(int argc, char **argv) {
    static const char pusage[] = "usage: rubrapack patch <base.msi> <target.msi> -o <out.msp> [--patch-code {GUID}] [--family <name>] [--no-removal]";
    const char *in[2] = { NULL, NULL }, *out = NULL;
    rp_msp_opts_t opts = { 0 };
    size_t nin = 0;
    for (int i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            out = argv[++i];
        } else if (strcmp(argv[i], "--patch-code") == 0 && i + 1 < argc) {
            opts.patch_code = argv[++i];
            size_t n = strlen(opts.patch_code);
            bool ok = n == 38 && opts.patch_code[0] == '{' && opts.patch_code[37] == '}';
            for (size_t k = 1; ok && k < 37; ++k) {
                char c = opts.patch_code[k];
                ok = (k == 9 || k == 14 || k == 19 || k == 24) ? c == '-' : ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'));
            }
            if (!ok) {
                rp_diag_error(RP_DIAG_EXTRA_ARGUMENT, "--patch-code takes a GUID in upper case with braces: {XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}");
                return RP_EXIT_USAGE;
            }
        } else if (strcmp(argv[i], "--family") == 0 && i + 1 < argc) {
            opts.family = argv[++i];
        } else if (strcmp(argv[i], "--no-removal") == 0) {
            opts.no_removal = true;
        } else if (argv[i][0] != '-' && nin < 2) {
            in[nin++] = argv[i];
        } else {
            rp_diag_error(RP_DIAG_EXTRA_ARGUMENT, "%s", pusage);
            return RP_EXIT_USAGE;
        }
    }
    if (nin != 2 || out == NULL) {
        rp_diag_error(RP_DIAG_EXTRA_ARGUMENT, "%s", pusage);
        return RP_EXIT_USAGE;
    }
    const char *slash = strrchr(out, '/'), *back = strrchr(out, '\\');
    opts.source_list = back && (!slash || back > slash) ? back + 1 : slash ? slash + 1 : out;
    proven_allocator_t heap = proven_heap_allocator();
    rp_limits_t limits = rp_limits_default();
    rp_pkg_t a, b;
    int rc = rp_pkg_open(heap, in[0], &limits, &a);
    if (rc == RP_EXIT_OK) rc = rp_pkg_open(heap, in[1], &limits, &b);
    else b = (rp_pkg_t){ 0 };
    if (rc == RP_EXIT_OK) {
        rp_mst_side_t sa = { &a.view.db, &a.cfb }, sb = { &b.view.db, &b.cfb };
        uint8_t *msp = NULL;
        size_t len = 0;
        const char *why = NULL;
        char what[128];
        proven_err_t err = rp_msp_write(heap, &sa, &sb, &opts, &limits, &msp, &len, &why, what, sizeof what);
        if (err == PROVEN_ERR_UNSUPPORTED && why) {
            if (what[0]) rp_diag_error(RP_DIAG_TRANSFORM, "%s %s", what, why);
            else rp_diag_error(RP_DIAG_TRANSFORM, "%s", why);
            rc = RP_EXIT_SOURCE;
        } else if (err != PROVEN_OK) {
            rp_diag_error(err == PROVEN_ERR_NOMEM ? RP_DIAG_NOMEM : RP_DIAG_BAD_PACKAGE, "cannot make a patch from '%s' to '%s'", in[0], in[1]);
            rc = RP_EXIT_IO;
        } else if (rp_pal_write_file_atomic(heap, out, msp, len) != PROVEN_OK) {
            rp_diag_error(RP_DIAG_OUTPUT, "cannot write '%s'", out);
            rc = RP_EXIT_IO;
        }
        rp_mem_free(heap, msp);
    }
    rp_pkg_close(heap, &a);
    rp_pkg_close(heap, &b);
    return rc;
}

// `rubrapack inspect <file.msp> --base <base.msi>`: the patch's own tables, then each transform in
// the order summary property 8 names them, listed against the base.
int rp_msp_inspect(const char *path, const char *base) {
    static const uint16_t summary_name[] = { 5, 'S', 'u', 'm', 'm', 'a', 'r', 'y', 'I', 'n', 'f', 'o', 'r', 'm', 'a', 't', 'i', 'o', 'n' };
    proven_allocator_t heap = proven_heap_allocator();
    rp_limits_t limits = rp_limits_default();
    rp_pkg_t p, b;
    int rc = rp_pkg_open(heap, path, &limits, &p);
    if (rc == RP_EXIT_OK) rc = rp_pkg_open(heap, base, &limits, &b);
    else b = (rp_pkg_t){ 0 };
    rp_buf_t out = rp_buf_new(heap, limits.max_metadata);
    for (size_t t = 0; rc == RP_EXIT_OK && t < p.msi.table_count; ++t) {
        uint8_t *idt = NULL;
        size_t n = 0;
        if (rp_msi_export_idt(&p.msi, t, &idt, &n) != PROVEN_OK) {
            rp_diag_error(RP_DIAG_BAD_PACKAGE, "'%s': a table is unreadable", path);
            rc = RP_EXIT_IO;
        } else {
            rp_buf_put(&out, idt, n);
        }
        rp_mem_free(heap, idt);
    }
    rp_suminfo_t si = { .count = 0 };
    uint32_t id;
    uint8_t *sum = NULL;
    size_t sn = 0;
    if (rc == RP_EXIT_OK && rp_cfb_find(&p.cfb, 0, summary_name, 19, &id) == PROVEN_OK) {
        sn = (size_t)p.cfb.entries[id].size;
        sum = rp_mem_alloc(heap, sn + 1, 1);
        if (sum == NULL || rp_cfb_read(&p.cfb, id, sum, sn) != PROVEN_OK || rp_suminfo_parse(sum, sn, &si) != PROVEN_OK) si.count = 0;
    }
    const rp_suminfo_prop_t *order = NULL;
    for (size_t i = 0; i < si.count; ++i) {
        if (si.props[i].pid == 8 && si.props[i].type == RP_VT_LPSTR) order = &si.props[i];
    }
    if (rc == RP_EXIT_OK && order == NULL) {
        rp_diag_error(RP_DIAG_BAD_PACKAGE, "'%s' names no transforms (summary property 8): not a patch", path);
        rc = RP_EXIT_IO;
    }
    for (size_t i = 0; rc == RP_EXIT_OK && i < order->str_len;) {
        size_t e = i;
        while (e < order->str_len && order->str[e] != ';') ++e;
        size_t s = i < e && order->str[i] == ':' ? i + 1 : i;
        uint16_t name[32];
        size_t nl = 0;
        for (size_t k = s; k < e && nl < 31; ++k) name[nl++] = order->str[k];
        const char *why = NULL;
        rp_buf_puts(&out, "== ");
        rp_buf_put(&out, order->str + s, e - s);
        rp_buf_byte(&out, '\n');
        if (rp_cfb_find(&p.cfb, 0, name, nl, &id) != PROVEN_OK || p.cfb.entries[id].type != RP_CFB_STORAGE) {
            rp_diag_error(RP_DIAG_BAD_PACKAGE, "'%s' has no transform '%.*s'", path, (int)(e - s), (const char *)order->str + s);
            rc = RP_EXIT_IO;
        } else if (rp_mst_list(heap, &p.cfb, id, &b.view.db, &limits, &out, &why) != PROVEN_OK) {
            rp_diag_error(RP_DIAG_BAD_PACKAGE, "'%s': transform '%.*s' cannot be read against '%s': %s", path, (int)(e - s),
                          (const char *)order->str + s, base, why && *why ? why : "unreadable");
            rc = RP_EXIT_IO;
        }
        i = e + 1;
    }
    if (rc == RP_EXIT_OK && (out.err != PROVEN_OK || rp_pal_write(RP_OUT_STDOUT, out.data, out.len) != PROVEN_OK)) rc = RP_EXIT_IO;
    rp_buf_free(&out);
    rp_mem_free(heap, sum);
    rp_pkg_close(heap, &p);
    rp_pkg_close(heap, &b);
    return rc;
}
