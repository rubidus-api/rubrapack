#ifndef RUBRAPACK_INSPECT_H
#define RUBRAPACK_INSPECT_H

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

#endif // RUBRAPACK_INSPECT_H
