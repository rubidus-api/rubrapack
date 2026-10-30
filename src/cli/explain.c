// src/cli/explain.c - `rubrapack explain [RPnnnn]` (RFC-0025): what a diagnostic code means, the
// messages that carry it (explain_table.c, generated from src/), what to do, and where the manual
// says more. Without a code: the code ranges.

#include "rubrapack/cli.h"
#include "rubrapack/diag.h"
#include "rubrapack/lang.h"
#include "rubrapack/inspect.h"
#include "rubrapack/pal.h"

#include "explain_int.h"

#include <stdio.h>
#include <string.h>

// The ranges, as the manual's table of codes has them (Reference, "Diagnostics").
typedef struct {
    const char *prefix, *what, *todo, *anchor, *section;
} range_t;

static const range_t ranges[] = {
    { "RP00", TR_N("the command line: an unknown command or option, a file that cannot be read or written, a signature, a difference a transform cannot carry"),
      TR_N("run `rubrapack help <command>`; check the paths and that the output folder exists"), TR_N("command-line"), TR_N("Command line") },
    { "RP10", TR_N("the source file's encoding"),
      TR_N("save the source as UTF-8 (or UTF-16 with a BOM), with LF or CRLF line ends"), TR_N("source-format"), TR_N("Source format") },
    { "RP11", TR_N("TOML outside the subset rubrapack reads, or `format` missing or too new"),
      TR_N("write the table or value the plain way the manual shows"), TR_N("the-toml-subset"), TR_N("The TOML subset") },
    { "RP12", TR_N("tables and keys: an unknown table or key, a required one missing, an item without a feature once features exist"),
      TR_N("check the spelling (the message suggests the nearest name) and the table's keys"), TR_N("tables"), TR_N("Tables") },
    { "RP13", TR_N("values: IDs, GUIDs, versions, numbers out of range, references to things that do not exist"),
      TR_N("fix the value the message names; IDs are letters, digits and '_' and differ across all tables"), TR_N("tables"), TR_N("Tables") },
    { "RP14", TR_N("variables: $(NAME) without a value, $( not closed, a Windows name or dir ID where it cannot stand"),
      TR_N("define the variable ([define] or -D NAME=value), or write $$ for a literal $"), TR_N("variables"), TR_N("Variables") },
    { "RP15", TR_N("the files to install: not found, a folder, a link, a glob without a match, another architecture's program, names too long"),
      TR_N("check the source paths (relative to the source file); set any-arch = true for another architecture on purpose"), TR_N("paths"), TR_N("Paths") },
    { "RP16", TR_N("MSIX: what an MSIX package needs, and what it cannot carry"),
      TR_N("give [msix] and [msix-app.*] what they need, or keep the item for the MSI with msi-only = true"), TR_N("msix-packages"), TR_N("MSIX packages") },
    { "RP19", TR_N("a feature this rubrapack does not provide (or was built without)"),
      TR_N("use a full rubrapack build or another way"), "", "" },
    { "RP20", TR_N("the finished tables break a Windows Installer rule"),
      TR_N("a source that passes the RP1xxx checks should never meet these: please report it with the source"), "", "" },
    { "RP21", TR_N("`lint` of a package: dialogs, code page, text normalisation"),
      TR_N("see the message; rebuilding with rubrapack fixes most of these"), TR_N("command-line"), TR_N("Command line (lint)") },
    { "RP22", TR_N("`lint` of an MSIX package or bundle"),
      TR_N("see the message; rebuilding with rubrapack fixes most of these"), TR_N("command-line"), TR_N("Command line (lint)") },
    { "RP23", TR_N("`lint --previous`: this package would not upgrade the previous one cleanly"),
      TR_N("keep the upgrade code, raise the version, keep component codes of files that stay"), "tutorial-03-versions-and-upgrades.html", TR_N("Versions and upgrades") },
};

static const range_t *range_of(const char *code) {
    for (size_t i = 0; i < sizeof ranges / sizeof ranges[0]; ++i) {
        if (strncmp(code, ranges[i].prefix, 4) == 0) return &ranges[i];
    }
    return NULL;
}

static void out(const char *s) { (void)rp_pal_puts(RP_OUT_STDOUT, s); }

// A message as the table of codes shows it: the text with "*" for each conversion.
static void starred(const char *fmt, bool translated, char *o, size_t cap) {
    size_t n = 0;
    for (const char *p = fmt; *p && n + 2 < cap; ++p) {
        if (!translated && *p == '%' && p[1] == '%') { o[n++] = '%'; ++p; continue; }
        if (!translated && *p == '%') {
            while (p[1] && strchr("diouxXscp", p[1]) == NULL) ++p;
            if (p[1]) ++p;
            o[n++] = '*';
            continue;
        }
        if (translated && *p == '{' && p[1] == '{') { o[n++] = '{'; ++p; continue; }
        if (translated && *p == '{' && (p[1] == '-' || (p[1] >= '1' && p[1] <= '9'))) {
            const char *q = p + 1;
            bool shown = *q != '-';
            if (!shown) ++q;
            while (*q >= '0' && *q <= '9') ++q;
            if (*q == '}') {
                if (shown) o[n++] = '*';
                p = q;
                continue;
            }
        }
        o[n++] = *p;
    }
    while (n && (o[n - 1] == ' ' || o[n - 1] == '\n')) --n;
    o[n] = '\0';
}

// The message `m` (starred English) in the chosen language, when a row's English reads the same.
static const char *in_language(const char *m, char *buf, size_t cap) {
    const rp_langpack_t *pk = rp_lang_pack();
    char en[1100];
    for (size_t i = 0; pk && i < pk->count; ++i) {
        if (pk->rows[i].ctxt) continue;
        starred(pk->rows[i].id, false, en, sizeof en);
        const char *s = en;
        while (*s == ' ') ++s;
        if (strcmp(s, m) == 0) {
            starred(pk->rows[i].str, true, buf, cap);
            return buf;
        }
    }
    return m;
}

int rp_cmd_explain(int argc, char **argv) {
    if (argc < 3) {
        out(TR("rubrapack explain <RPnnnn>: what a diagnostic code means. The codes by range:\n"));
        char line[512];
        for (size_t i = 0; i < sizeof ranges / sizeof ranges[0]; ++i) {
            snprintf(line, sizeof line, "  %sxx  %s\n", ranges[i].prefix, rp_tr(ranges[i].what));
            out(line);
        }
        return RP_EXIT_OK;
    }
    if (argc > 3) {
        (void)rp_cli_usage_error("explain");
        return RP_EXIT_USAGE;
    }
    // "RP1612", "rp1612" and "1612" alike.
    const char *a = argv[2];
    char code[8];
    if ((a[0] == 'R' || a[0] == 'r') && (a[1] == 'P' || a[1] == 'p')) a += 2;
    if (strlen(a) != 4 || strspn(a, "0123456789") != 4) {
        rp_diag_error(RP_DIAG_BAD_ARG_TEXT, "'%s' is not a diagnostic code (RP and four digits, like RP1612)", argv[2]);
        return RP_EXIT_USAGE;
    }
    snprintf(code, sizeof code, "RP%s", a);
    const rp_explain_entry_t *e = NULL;
    for (size_t i = 0; i < rp_explain_count; ++i) {
        if (strcmp(rp_explain_table[i].code, code) == 0) e = &rp_explain_table[i];
    }
    const range_t *r = range_of(code);
    if (e == NULL) {
        rp_diag_error(RP_DIAG_BAD_ARG_TEXT, "%s is not a code this rubrapack uses%s%s", code, r ? TR("; its range is ") : "", r ? rp_tr(r->what) : "");
        return RP_EXIT_USAGE;
    }
    char line[1024];
    snprintf(line, sizeof line, "%s - %s\n", code, r ? rp_tr(r->what) : TR("a diagnostic of rubrapack"));
    out(line);
    if (e->count) {
        out(TR("\nMessages with this code (* stands for a name or a value):\n"));
        for (size_t i = 0; i < e->count; ++i) {
            char tr[1000];
            snprintf(line, sizeof line, "  - %s\n", in_language(e->messages[i], tr, sizeof tr));
            out(line);
        }
    }
    if (r) {
        rp_lang_format(line, sizeof line, "\nWhat to do: %s.\n", rp_tr(r->todo));
        out(line);
        if (r->anchor[0]) {
            bool page = strstr(r->anchor, ".html") != NULL;     // a tutorial chapter, or a section of the reference
            // The address is part of the text: a language's pack names its own manual (and anchors).
            rp_lang_format(line, sizeof line, "Manual: %s\"%s\" - https://rubidus-api.github.io/rubrapack/en/%s%s\n", page ? "" : TR("Reference, "),
                           rp_tr(r->section), page ? "" : "reference.html#", page ? r->anchor : rp_tr(r->anchor));
            out(line);
        }
        out(TR("All codes: https://rubidus-api.github.io/rubrapack/en/reference.html#diagnostic-codes\n"));
    }
    return RP_EXIT_OK;
}
