#ifndef RUBRAPACK_INSPECT_H
#define RUBRAPACK_INSPECT_H

// include/rubrapack/inspect.h - the `inspect` command.

// argv as rp_main receives it (argv[1] == "inspect"); returns the exit code.
[[nodiscard]] int rp_cmd_inspect(int argc, char **argv);

#endif // RUBRAPACK_INSPECT_H
