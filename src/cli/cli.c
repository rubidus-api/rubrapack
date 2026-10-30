// src/cli/cli.c - command dispatch (RFC-0001 section 7). A planned command that is not built
// yet is an error, never a silent success (section 7.1).

#include "rubrapack/cli.h"
#include "rubrapack/diag.h"
#include "rubrapack/inspect.h"
#include "rubrapack/lang.h"
#include "rubrapack/mem.h"
#include "rubrapack/pal.h"
#include "rubrapack/text.h"
#include "rubrapack/version.h"

#include <stdio.h>
#include <string.h>

#include "proven/heap.h"
#include "proven/version.h"

typedef struct {
    const char *name;
    const char *usage;
    const char *summary;
    bool        implemented;
} command_t;

static const command_t commands[] = {
    { "build",   "build <src.toml> -o <out.msi|out.msm|out.msix|out.msixbundle|out.msixupload|setup.exe> [-D NAME=VALUE] [--arch x64|arm64|x86 (a list for a bundle)] [--compress none] [--jobs N] [--nfc] [--reproducible] [(--key <key.pfx|.pem> [--pass-env VAR | --pass-file FILE] | --pkcs11 <module> --key-label <label> [--token-label <label>] [--pin-env VAR | --pin-file FILE] | --key-store <thumbprint> [--machine-store]) [--cert <chain.pem>] [--timestamp <URL> [--tsa-trust <certificates>] [--tls-trust <certificates>] [--system-roots] [--proxy <URL>]] [--allow-unsigned-cabs]] [--unsigned-test] [--msix-compress deflate|store] [--flat] [--winget DIR]",
                 TR_N("build a package from a source file"), true },
    { "lint",    "lint <src.toml> [-D NAME=VALUE] [--arch x64|arm64|x86] [--target msi|msix] [--nfc] [--strict] [--json] | lint <file.msi> [--previous <old.msi>] [--strict] [--json] | lint <file.msix|file.msixbundle> [--strict] [--json]",
                 TR_N("check a source or package without writing anything"), true },
    { "sign",    "sign <file.exe|.dll|.msi|.msp|.msix|.msixbundle|.cab> (--key <key.pfx|.pem> [--pass-env VAR | --pass-file FILE] | --pkcs11 <module> --key-label <label> [--token-label <label>] [--pin-env VAR | --pin-file FILE] | --key-store <thumbprint> [--machine-store]) [--cert <chain.pem>] [--timestamp <URL> [--tsa-trust <certificates>] [--tls-trust <certificates>] [--system-roots] [--proxy <URL>]] [--allow-unsigned-cabs] [-o <out>]",
                 TR_N("sign a PE file, an MSI package, an MSIX package or bundle"), true },
    { "verify",  "verify <file.exe|.dll|.msi|.msp|.msix|.msixbundle|.cab> [--trust <certificates>]... [--system-roots] [--tsa-trust <certificates>]...",
                 TR_N("check a signature: structure, digest, signature, and the path to a trusted certificate"), true },
    { "keys",    "keys list [--pkcs11 <module> [--token-label <label>] [--pin-env VAR | --pin-file FILE]]",
                 TR_N("list the keys a token or the Windows certificate store can sign with"), true },
    { "inspect", "inspect <file.msi> [table|--summary|--files|--streams] | inspect <file.msi> [table|--summary] --json | inspect <file.msix|file.msixbundle> [--files|--manifest] | inspect <file.mst|file.msp> --base <base.msi> | inspect <file.mst> --summary|--streams | inspect <file.cab> | inspect <setup.exe>",
                 TR_N("dump a package (MSI tables as IDT)"), true },
    { "extract", "extract <file.msi|file.msix|file.msixbundle|file.cab|setup.exe> -d <new dir> [--limit-entries N] [--limit-bytes N]",
                 TR_N("unpack a package into a new directory, laid out as it installs"), true },
    { "diff",    "diff <a.msi> <b.msi>", TR_N("show what differs between two packages: tables, rows, cells and summary"), true },
    { "transform", "transform <base.msi> <target.msi> -o <out.mst> [--validate none|product-code,upgrade-code,language,platform]",
                 TR_N("write a transform (.mst): what turns one package into the other"), true },
    { "patch",   "patch <base.msi> <target.msi> -o <out.msp> [--patch-code {GUID}] [--family <name>] [--no-removal]",
                 TR_N("write a patch (.msp): the files and rows that update an installed base to the target"), true },
    { "new",     "new [<name>] [-i] | new <name> --dist <folder> [--name <text>] [--manufacturer <text>] [--version <a.b.c>] [--arch x64|x86|arm64] [--main <file>|-] [--install-dir <folder>] [--scope machine|user|dual] [--ui none|basic|minimal|installdir|features] [--license <file>|-] [--languages ko|-] [--optional <folder,...>|-] [--shortcuts start,desktop|none] | new <name> | new <name> --from <package.msi> [--dist <new folder>] [--publisher \"CN=...\"]", TR_N("write a source file: made from answers, asked or given as options, a fixed starter, or an existing MSI"), true },
    { "edit",    "edit <file>.toml [--set table.key=value]... [--unset table.key]... [--sync]", TR_N("change a source in place: through a menu of questions, or with the options"), true },
    { "guid",    "guid [--from <text>]", TR_N("print a random GUID, or the one rubrapack derives from a text"), true },
    { "schema",  "schema", TR_N("print the JSON Schema of sources (for an editor that completes and checks keys)"), true },
    { "explain", "explain [RPnnnn]", TR_N("what a diagnostic code means, the messages that carry it, what to do and where the manual says more"), true },
    { "try",     "try <package.msi|package.msix|package.msixbundle> [-o <out.wsb>] [--host <folder>] [--network]",
                 TR_N("write a Windows Sandbox configuration that installs the package in a fresh, disposable Windows"), true },
    { "container", "container <name> <package.msix|package.msixbundle|family name>... -o <out.xml>",
                 TR_N("write a shared package container's definition: MSIX packages that run in one container"), true },
    { "installed", "installed <{upgrade code}|package family name>",
                 TR_N("say whether this Windows has a product of that upgrade code or an app package of that family installed"), true },
    { "version", "version", TR_N("print the version"), true },
    { "help",    "help [command]", TR_N("print this help, or one command's usage"), true },
};

enum { COMMAND_COUNT = sizeof commands / sizeof commands[0] };

const char *rp_cli_usage(const char *command) {
    for (size_t i = 0; i < COMMAND_COUNT; ++i) {
        if (strcmp(commands[i].name, command) == 0) return commands[i].usage;
    }
    return command;
}

int rp_cli_usage_error(const char *command) {
    rp_diag_error(RP_DIAG_EXTRA_ARGUMENT, "usage: rubrapack %s", rp_cli_usage(command));
    return RP_EXIT_USAGE;
}

static const command_t *find_command(const char *name) {
    for (size_t i = 0; i < COMMAND_COUNT; ++i) {
        if (strcmp(commands[i].name, name) == 0) return &commands[i];
    }
    return nullptr;
}

// The languages this rubrapack has built in, for the error of a code it does not know: "en, ko, ...".
static const char *built_in_langs(char *out, size_t cap) {
    size_t o = (size_t)snprintf(out, cap, "en");
    for (size_t i = 0; i < rp_lang_embedded_count && o < cap; ++i)
        o += (size_t)snprintf(out + o, cap - o, ", %s", rp_lang_embedded[i].code);
    return out;
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
    proven_err_t err = rp_pal_puts(out, "rubrapack " RUBRAPACK_VERSION_STRING);
    if (err == PROVEN_OK) {
        err = rp_pal_puts(out, TR(" - build Windows Installer (.msi) and MSIX (.msix) packages\n"
                                  "\n"
                                  "usage: rubrapack [--lang <code>] <command> [arguments]\n"
                                  "\n"
                                  "commands:\n"));
    }
    for (size_t i = 0; i < COMMAND_COUNT && err == PROVEN_OK; ++i) {
        snprintf(line, sizeof line, "  %-9s %s%s\n", commands[i].name, rp_tr(commands[i].summary),
                 commands[i].implemented ? "" : " (not implemented yet)");
        err = rp_pal_puts(out, line);
    }
    if (err == PROVEN_OK) {
        err = rp_pal_puts(out, TR("\n"
                                  "Results go to stdout, diagnostics to stderr.\n"));
    }
    if (err == PROVEN_OK) {
        char list[256], text[1024];
        rp_lang_format(text, sizeof text, "--lang (or RUBRAPACK_LANG) chooses the language of these texts: %s, or one with a language pack.\n",
                       built_in_langs(list, sizeof list));
        err = rp_pal_puts(out, text);
    }
    if (err == PROVEN_OK) {
        err = rp_pal_puts(out, TR("Exit codes: 0 ok, 1 source error, 2 usage, 3 input/output, 4 signing or verification,\n"
                                  "5 lint, 6 network.\n"));
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
    char line[2048];                    // the longest usage (build) is over 600 bytes
    rp_lang_format(line, sizeof line, "usage: rubrapack %s\n\n%s%s\n", c->usage, rp_tr(c->summary),
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

    // The language of the texts: RUBRAPACK_LANG, then --lang <code> (or --lang=<code>) anywhere on
    // the line, which is taken out here so that no command sees it and a usage error is already in
    // that language. English when neither is given; it never changes what is built.
    {
        proven_allocator_t heap = proven_heap_allocator();
        char *env = rp_pal_getenv(heap, "RUBRAPACK_LANG");
        if (env && env[0] && !rp_lang_set(env)) {
            char buf[64], buf2[64], list[256];
            rp_diag_error(RP_DIAG_BAD_ARG_TEXT, "RUBRAPACK_LANG=%s: no such language (built in: %s; another needs its language pack, %s.po)",
                          shown(env, buf, sizeof buf), built_in_langs(list, sizeof list), shown(env, buf2, sizeof buf2));
            rp_mem_free(heap, env);
            return RP_EXIT_USAGE;
        }
        if (env) rp_mem_free(heap, env);
    }
    int kept = 1;
    for (int i = 1; i < argc; ++i) {
        const char *code = NULL;
        if (strcmp(argv[i], "--lang") == 0) {
            if (i + 1 >= argc) {
                rp_diag_error(RP_DIAG_BAD_ARG_TEXT, "--lang needs a language, such as en or ko");
                return RP_EXIT_USAGE;
            }
            code = argv[++i];
        } else if (strncmp(argv[i], "--lang=", 7) == 0) {
            code = argv[i] + 7;
        } else {
            argv[kept++] = argv[i];
            continue;
        }
        if (!rp_lang_set(code)) {
            char buf[64], buf2[64], list[256];
            rp_diag_error(RP_DIAG_BAD_ARG_TEXT, "--lang %s: no such language (built in: %s; another needs its language pack, %s.po)",
                          shown(code, buf, sizeof buf), built_in_langs(list, sizeof list), shown(code, buf2, sizeof buf2));
            return RP_EXIT_USAGE;
        }
    }
    argc = kept;
    argv[argc] = NULL;

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
    if (strcmp(name, "edit") == 0) return rp_cmd_edit(argc, argv);
    if (strcmp(name, "guid") == 0) return rp_cmd_guid(argc, argv);
    if (strcmp(name, "explain") == 0) return rp_cmd_explain(argc, argv);
    if (strcmp(name, "schema") == 0) return rp_cmd_schema(argc, argv);
    if (strcmp(name, "sign") == 0) return rp_cmd_sign(argc, argv);
    if (strcmp(name, "keys") == 0) return rp_cmd_keys(argc, argv);
    if (strcmp(name, "verify") == 0) return rp_cmd_verify(argc, argv);
    if (strcmp(name, "transform") == 0) return rp_cmd_transform(argc, argv);
    if (strcmp(name, "diff") == 0) return rp_cmd_diff(argc, argv);
    if (strcmp(name, "container") == 0) return rp_cmd_container(argc, argv);
    if (strcmp(name, "try") == 0) return rp_cmd_try(argc, argv);
    if (strcmp(name, "installed") == 0) return rp_cmd_installed(argc, argv);
    if (strcmp(name, "patch") == 0) return rp_cmd_patch(argc, argv);

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
