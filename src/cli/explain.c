// src/cli/explain.c - `rubrapack explain [RPnnnn]` (RFC-0025): what a diagnostic code means, the
// messages that carry it (explain_table.c, generated from src/), what to do, and where the manual
// says more. Without a code: the code ranges.

#include "rubrapack/diag.h"
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
    { "RP00", "the command line: an unknown command or option, a file that cannot be read or written, a signature, a difference a transform cannot carry",
      "run `rubrapack help <command>`; check the paths and that the output folder exists", "command-line", "Command line" },
    { "RP10", "the source file's encoding", "save the source as UTF-8 (or UTF-16 with a BOM), with LF or CRLF line ends", "source-format", "Source format" },
    { "RP11", "TOML outside the subset rubrapack reads, or `format` missing or too new", "write the table or value the plain way the manual shows",
      "the-toml-subset", "The TOML subset" },
    { "RP12", "tables and keys: an unknown table or key, a required one missing, an item without a feature once features exist",
      "check the spelling (the message suggests the nearest name) and the table's keys", "tables", "Tables" },
    { "RP13", "values: IDs, GUIDs, versions, numbers out of range, references to things that do not exist",
      "fix the value the message names; IDs are letters, digits and '_' and differ across all tables", "tables", "Tables" },
    { "RP14", "variables: $(NAME) without a value, $( not closed, a Windows name or dir ID where it cannot stand",
      "define the variable ([define] or -D NAME=value), or write $$ for a literal $", "variables", "Variables" },
    { "RP15", "the files to install: not found, a folder, a link, a glob without a match, another architecture's program, names too long",
      "check the source paths (relative to the source file); set any-arch = true for another architecture on purpose", "paths", "Paths" },
    { "RP16", "MSIX: what an MSIX package needs, and what it cannot carry",
      "give [msix] and [msix-app.*] what they need, or keep the item for the MSI with msi-only = true", "msix-packages", "MSIX packages" },
    { "RP19", "a feature this rubrapack does not provide (or was built without)", "use a full rubrapack build or another way", "", "" },
    { "RP20", "the finished tables break a Windows Installer rule",
      "a source that passes the RP1xxx checks should never meet these: please report it with the source", "", "" },
    { "RP21", "`lint` of a package: dialogs, code page, text normalisation", "see the message; rebuilding with rubrapack fixes most of these",
      "command-line", "Command line (lint)" },
    { "RP22", "`lint` of an MSIX package or bundle", "see the message; rebuilding with rubrapack fixes most of these", "command-line", "Command line (lint)" },
    { "RP23", "`lint --previous`: this package would not upgrade the previous one cleanly",
      "keep the upgrade code, raise the version, keep component codes of files that stay", "tutorial-03-versions-and-upgrades.html", "Versions and upgrades" },
};

static const range_t *range_of(const char *code) {
    for (size_t i = 0; i < sizeof ranges / sizeof ranges[0]; ++i) {
        if (strncmp(code, ranges[i].prefix, 4) == 0) return &ranges[i];
    }
    return NULL;
}

static void out(const char *s) { (void)rp_pal_puts(RP_OUT_STDOUT, s); }

int rp_cmd_explain(int argc, char **argv) {
    if (argc < 3) {
        out("rubrapack explain <RPnnnn>: what a diagnostic code means. The codes by range:\n");
        char line[512];
        for (size_t i = 0; i < sizeof ranges / sizeof ranges[0]; ++i) {
            snprintf(line, sizeof line, "  %sxx  %s\n", ranges[i].prefix, ranges[i].what);
            out(line);
        }
        return RP_EXIT_OK;
    }
    if (argc > 3) {
        rp_diag_error(RP_DIAG_EXTRA_ARGUMENT, "usage: rubrapack explain [RPnnnn]");
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
        rp_diag_error(RP_DIAG_BAD_ARG_TEXT, "%s is not a code this rubrapack uses%s%s", code, r ? "; its range is " : "", r ? r->what : "");
        return RP_EXIT_USAGE;
    }
    char line[1024];
    snprintf(line, sizeof line, "%s - %s\n", code, r ? r->what : "a diagnostic of rubrapack");
    out(line);
    if (e->count) {
        out("\nMessages with this code (* stands for a name or a value):\n");
        for (size_t i = 0; i < e->count; ++i) {
            snprintf(line, sizeof line, "  - %s\n", e->messages[i]);
            out(line);
        }
    }
    if (r) {
        snprintf(line, sizeof line, "\nWhat to do: %s.\n", r->todo);
        out(line);
        if (r->anchor[0]) {
            bool page = strstr(r->anchor, ".html") != NULL;     // a tutorial chapter, or a section of the reference
            snprintf(line, sizeof line, "Manual: %s\"%s\" - https://rubidus-api.github.io/rubrapack/en/%s%s\n", page ? "" : "Reference, ", r->section,
                     page ? "" : "rpk.html#", r->anchor);
            out(line);
        }
        out("All codes: https://rubidus-api.github.io/rubrapack/en/rpk.html#diagnostic-codes\n");
    }
    return RP_EXIT_OK;
}
