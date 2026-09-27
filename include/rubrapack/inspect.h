#ifndef RUBRAPACK_INSPECT_H
#define RUBRAPACK_INSPECT_H

#include <stddef.h>
#include <stdint.h>

// include/rubrapack/inspect.h - the `inspect` and `build` commands.

// argv as rp_main receives it (argv[1] == "inspect"); returns the exit code.
[[nodiscard]] int rp_cmd_inspect(int argc, char **argv);

// `rubrapack build` (src/cli/build.c).
[[nodiscard]] int rp_cmd_build(int argc, char **argv);

// `rubrapack lint <src.rpk|file.msi> [--strict]` (RFC-0006 1); the source half lives in build.c.
[[nodiscard]] int rp_cmd_lint(int argc, char **argv);
[[nodiscard]] int rp_cmd_lint_source(int argc, char **argv);

// `rubrapack extract <file.msi|file.cab> -d <new dir>` (RFC-0006 2).
[[nodiscard]] int rp_cmd_extract(int argc, char **argv);

// `rubrapack new [msi] <name>` and `rubrapack guid [--from <text>]` (RFC-0006 5).
[[nodiscard]] int rp_cmd_new(int argc, char **argv);
[[nodiscard]] int rp_cmd_guid(int argc, char **argv);

// Signing options shared by `sign` and `build --key` (one code path, RFC-0007 S4).
typedef struct {
    const char *key, *cert, *pass_env, *pass_file;
    bool        allow_unsigned_cabs;
} rp_sign_args_t;

// Signs a PE file or an MSI package held in memory; prints its own diagnostics (`label` names the
// file) and returns an exit code.
[[nodiscard]] int rp_sign_bytes(const rp_sign_args_t *a, const char *label, const uint8_t *in, size_t len, uint8_t **out, size_t *out_len);

// `rubrapack sign` and `rubrapack verify` (RFC-0007 S4).
[[nodiscard]] int rp_cmd_sign(int argc, char **argv);
[[nodiscard]] int rp_cmd_verify(int argc, char **argv);

#endif // RUBRAPACK_INSPECT_H
