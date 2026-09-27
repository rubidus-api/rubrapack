// src/cli/cli.c - command dispatch (RFC-0001 section 7). A planned command that is not built
// yet is an error, never a silent success (section 7.1).

#include "rubrapack/cli.h"
#include "rubrapack/diag.h"
#include "rubrapack/inspect.h"
#include "rubrapack/pal.h"
#include "rubrapack/text.h"
#include "rubrapack/version.h"

#include <stdio.h>
#include <string.h>

#include "proven/version.h"

typedef struct {
    const char *name;
    const char *usage;
    const char *summary;
    bool        implemented;
} command_t;

static const command_t commands[] = {
    { "build",   "build <src.rpk> -o <out.msi> [-D NAME=VALUE] [--arch x64|arm64|x86] [--compress none] [--nfc] [--reproducible]",
                 "build a package from a source file", true },
    { "sign",    "sign <file.exe|.dll> --key <key.pfx|.pem> [--cert <chain.pem>] [--pass-env VAR | --pass-file FILE] [-o <out>]",
                 "sign a PE file (MSI follows)", true },
    { "verify",  "verify <file.exe|.dll> [--trust <certificate>]...",
                 "check a signature: structure, digest, signature, and the path to a trusted certificate", true },
    { "inspect", "inspect <file.msi> [table|--summary|--files|--streams] | inspect <file.cab>",
                 "dump a package (MSI tables as IDT)", true },
    { "extract", "extract <file.msi|file.cab> -d <new dir> [--limit-entries N] [--limit-bytes N]",
                 "unpack a package into a new directory, laid out as it installs", true },
    { "lint",    "lint <src.rpk> [-D NAME=VALUE] [--arch x64|arm64|x86] [--strict] | lint <file.msi> [--strict]",
                 "check a source or package without writing anything", true },
    { "new",     "new [msi] <name>", "write a starter source file <name>.rpk", true },
    { "guid",    "guid [--from <text>]", "print a random GUID, or the one rubrapack derives from a text", true },
    { "keys",    "keys list [--store|--pkcs11 <module>]", "list usable signing keys", false },
    { "version", "version", "print the version", true },
    { "help",    "help [command]", "print this help, or one command's usage", true },
};

enum { COMMAND_COUNT = sizeof commands / sizeof commands[0] };

static const command_t *find_command(const char *name) {
    for (size_t i = 0; i < COMMAND_COUNT; ++i) {
        if (strcmp(commands[i].name, name) == 0) return &commands[i];
    }
    return nullptr;
}

// Copies a user argument for a diagnostic: control characters become \xNN, and a long
// argument is cut on a character boundary and marked with "...". `arg` is valid UTF-8.
static const char *shown(const char *arg, char *out, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; arg[i] != '\0';) {
        unsigned char c = (unsigned char)arg[i];
        size_t n = c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2;
        bool control = c < 0x20 || c == 0x7F;
        size_t width = control ? 4 : n;
        if (o + width + 4 > cap) {
            memcpy(out + o, "...", 4);
            return out;
        }
        if (control) {
            snprintf(out + o, cap - o, "\\x%02X", c);
        } else {
            memcpy(out + o, arg + i, n);
        }
        o += width;
        i += n;
    }
    out[o] = '\0';
    return out;
}

static int print_usage(rp_out_t out) {
    char line[256];
    proven_err_t err = rp_pal_puts(out,
        "rubrapack " RUBRAPACK_VERSION_STRING
        " - build Windows Installer (.msi) and MSIX (.msix) packages\n"
        "\n"
        "usage: rubrapack <command> [arguments]\n"
        "\n"
        "commands:\n");
    for (size_t i = 0; i < COMMAND_COUNT && err == PROVEN_OK; ++i) {
        snprintf(line, sizeof line, "  %-8s %s%s\n", commands[i].name, commands[i].summary,
                 commands[i].implemented ? "" : " (not implemented yet)");
        err = rp_pal_puts(out, line);
    }
    if (err == PROVEN_OK) {
        err = rp_pal_puts(out,
            "\n"
            "Results go to stdout, diagnostics to stderr.\n"
            "Exit codes: 0 ok, 1 source error, 2 usage, 3 input/output, 4 signing or verification,\n"
            "5 lint, 6 network.\n");
    }
    return err == PROVEN_OK ? RP_EXIT_OK : RP_EXIT_IO;
}

static int cmd_version(int argc) {
    if (argc > 2) {
        rp_diag_error(RP_DIAG_EXTRA_ARGUMENT, "'version' takes no arguments");
        return RP_EXIT_USAGE;
    }
    proven_err_t err = rp_pal_puts(RP_OUT_STDOUT,
        "rubrapack " RUBRAPACK_VERSION_STRING " (" PROVEN_VERSION_STRING ")\n");
    return err == PROVEN_OK ? RP_EXIT_OK : RP_EXIT_IO;
}

static int cmd_help(int argc, char **argv) {
    char buf[256];
    if (argc == 2) return print_usage(RP_OUT_STDOUT);
    if (argc > 3) {
        rp_diag_error(RP_DIAG_EXTRA_ARGUMENT, "'help' takes at most one command name");
        return RP_EXIT_USAGE;
    }
    const command_t *c = find_command(argv[2]);
    if (c == nullptr) {
        rp_diag_error(RP_DIAG_UNKNOWN_COMMAND, "unknown command '%s' (see 'rubrapack help')",
                      shown(argv[2], buf, sizeof buf));
        return RP_EXIT_USAGE;
    }
    char line[512];
    snprintf(line, sizeof line, "usage: rubrapack %s\n\n%s%s\n", c->usage, c->summary,
             c->implemented ? "" : " (not implemented yet)");
    return rp_pal_puts(RP_OUT_STDOUT, line) == PROVEN_OK ? RP_EXIT_OK : RP_EXIT_IO;
}

int rp_main(int argc, char **argv) {
    for (int i = 1; i < argc; ++i) {
        rp_text_result_t r = rp_utf8_validate((const uint8_t *)argv[i], strlen(argv[i]));
        if (r.err != PROVEN_OK) {
            rp_diag_error(RP_DIAG_BAD_ARG_TEXT, "argument %d is not valid UTF-8 (byte %zu)", i,
                          r.offset);
            return RP_EXIT_USAGE;
        }
    }

    if (argc < 2) {
        (void)print_usage(RP_OUT_STDERR);
        return RP_EXIT_USAGE;
    }

    const char *name = argv[1];
    if (strcmp(name, "--version") == 0) name = "version";
    if (strcmp(name, "--help") == 0 || strcmp(name, "-h") == 0) name = "help";

    if (strcmp(name, "version") == 0) return cmd_version(argc);
    if (strcmp(name, "help") == 0) return cmd_help(argc, argv);
    if (strcmp(name, "inspect") == 0) return rp_cmd_inspect(argc, argv);
    if (strcmp(name, "build") == 0) return rp_cmd_build(argc, argv);
    if (strcmp(name, "lint") == 0) return rp_cmd_lint(argc, argv);
    if (strcmp(name, "extract") == 0) return rp_cmd_extract(argc, argv);
    if (strcmp(name, "new") == 0) return rp_cmd_new(argc, argv);
    if (strcmp(name, "guid") == 0) return rp_cmd_guid(argc, argv);
    if (strcmp(name, "sign") == 0) return rp_cmd_sign(argc, argv);
    if (strcmp(name, "verify") == 0) return rp_cmd_verify(argc, argv);

    char buf[256];
    const command_t *c = find_command(name);
    if (c == nullptr) {
        rp_diag_error(RP_DIAG_UNKNOWN_COMMAND, "unknown command '%s' (see 'rubrapack help')",
                      shown(name, buf, sizeof buf));
        return RP_EXIT_USAGE;
    }
    rp_diag_error(RP_DIAG_NOT_IMPLEMENTED, "command '%s' is not implemented yet", c->name);
    return RP_EXIT_USAGE;
}
