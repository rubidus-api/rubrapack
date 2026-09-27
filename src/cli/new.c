// src/cli/new.c - `rubrapack new [msi] <name>` and `rubrapack guid [--from <text>]` (RFC-0006 5).

#include "rubrapack/diag.h"
#include "rubrapack/ident.h"
#include "rubrapack/inspect.h"
#include "rubrapack/mem.h"
#include "rubrapack/pal.h"
#include "rubrapack/text.h"

#include <stdio.h>
#include <string.h>

#include "proven/heap.h"

// A product name that is also a file name and a TOML string without escapes.
static const char *name_problem(const char *s) {
    size_t n = strlen(s);
    if (n == 0 || n > 64) return "must be 1 to 64 bytes";
    if (rp_utf8_validate((const uint8_t *)s, n).err != PROVEN_OK) return "is not valid UTF-8";
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c == 0x7F || strchr("/\\:*?\"<>|$", c)) return "may not contain control characters or / \\ : * ? \" < > | $";
    }
    if (s[0] == ' ' || s[n - 1] == ' ' || s[n - 1] == '.') return "may not start with a space or end with a space or a dot";
    return NULL;
}

int rp_cmd_new(int argc, char **argv) {
    const char *kind = "msi", *name = NULL;
    if (argc == 3) name = argv[2];
    else if (argc == 4) {
        kind = argv[2];
        name = argv[3];
    }
    if (name == NULL) {
        rp_diag_error(RP_DIAG_EXTRA_ARGUMENT, "usage: rubrapack new [msi] <name>");
        return RP_EXIT_USAGE;
    }
    if (strcmp(kind, "msix") == 0) {
        rp_diag_error(RP_DIAG_NOT_IMPLEMENTED, "new msix is not implemented yet (MSIX is planned for P8)");
        return RP_EXIT_USAGE;
    }
    if (strcmp(kind, "msi") != 0) {
        rp_diag_error(RP_DIAG_EXTRA_ARGUMENT, "usage: rubrapack new [msi] <name>");
        return RP_EXIT_USAGE;
    }
    const char *why = name_problem(name);
    if (why) {
        rp_diag_error(RP_DIAG_BAD_ARG_TEXT, "the name %s", why);
        return RP_EXIT_USAGE;
    }
    char code[39];
    if (rp_uuid_random(code) != PROVEN_OK) {
        rp_diag_error(RP_DIAG_OUTPUT, "the system random source failed");
        return RP_EXIT_IO;
    }
    // The summary information is ASCII (RFC-0001 9.10): an ASCII form of the name, when needed.
    char ascii[65];
    size_t na = 0;
    bool all_ascii = true;
    for (const char *s = name; *s; ++s) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x80) ascii[na++] = (char)c;
        else all_ascii = false;
    }
    while (na && ascii[na - 1] == ' ') --na;
    ascii[na] = '\0';
    char summary[192] = "";
    if (!all_ascii) snprintf(summary, sizeof summary, "summary-name = \"%s\"         # ASCII: the file properties show this\n",
                             na ? ascii : "package");
    char text[4096];
    int len = snprintf(text, sizeof text,
        "# %s.rpk - made by `rubrapack new`. Put the program's files in dist/ next to this file, then:\n"
        "#   rubrapack build \"%s.rpk\" -o \"%s.msi\"\n"
        "# Every table and key is described in manual/rpk.md of the rubrapack sources.\n"
        "\n"
        "[package]\n"
        "name = \"%s\"\n"
        "%s"
        "manufacturer = \"%s authors\"     # shown in Settings > Installed apps\n"
        "version = \"1.0.0\"\n"
        "arch = \"x64\"                      # x64, arm64 or x86\n"
        "upgrade-code = \"%s\"   # keep it in every later version: it ties the upgrades together\n"
        "language = \"en-US\"                # or \"ko-KR\"\n"
        "\n"
        "[dir.INSTALLDIR]\n"
        "path = \"ProgramFiles/%s\"\n"
        "\n"
        "[files.App]\n"
        "dir = \"INSTALLDIR\"\n"
        "glob = \"dist/*\"\n",
        name, name, name, name, summary, name, code, name);
    if (len < 0 || (size_t)len >= sizeof text) return RP_EXIT_IO;
    char path[128];
    snprintf(path, sizeof path, "%s.rpk", name);
    proven_err_t err = rp_pal_write_file_new(proven_heap_allocator(), path, (const uint8_t *)text, (size_t)len);
    if (err == PROVEN_ERR_BUSY) {
        rp_diag_error(RP_DIAG_OUTPUT, "'%s' already exists; it is left as it is", path);
        return RP_EXIT_IO;
    }
    if (err != PROVEN_OK) {
        rp_diag_error(RP_DIAG_OUTPUT, "cannot write '%s'", path);
        return RP_EXIT_IO;
    }
    char line[200];
    snprintf(line, sizeof line, "wrote %s\n", path);
    return rp_pal_puts(RP_OUT_STDOUT, line) == PROVEN_OK ? RP_EXIT_OK : RP_EXIT_IO;
}

// `guid`: a random UUIDv4. `guid --from <text>`: the UUIDv8 rubrapack derives (include/rubrapack/
// ident.h) with the tag "guid" and the text as its one field (RFC-0006 L5).
int rp_cmd_guid(int argc, char **argv) {
    char out[39];
    if (argc == 2) {
        if (rp_uuid_random(out) != PROVEN_OK) {
            rp_diag_error(RP_DIAG_OUTPUT, "the system random source failed");
            return RP_EXIT_IO;
        }
    } else if (argc == 4 && strcmp(argv[2], "--from") == 0) {
        const char *fields[1] = { argv[3] };
        rp_uuid_derive("guid", fields, 1, out);
    } else {
        rp_diag_error(RP_DIAG_EXTRA_ARGUMENT, "usage: rubrapack guid [--from <text>]");
        return RP_EXIT_USAGE;
    }
    char line[48];
    snprintf(line, sizeof line, "%s\n", out);
    return rp_pal_puts(RP_OUT_STDOUT, line) == PROVEN_OK ? RP_EXIT_OK : RP_EXIT_IO;
}
