#ifndef RUBRAPACK_CLI_H
#define RUBRAPACK_CLI_H

// include/rubrapack/cli.h - the command line, after the PAL has made argv UTF-8.

// Runs one rubrapack command and returns the process exit code (include/rubrapack/diag.h).
// Every argument must be UTF-8; an argument that is not is a usage error.
[[nodiscard]] int rp_main(int argc, char **argv);

// A command's usage line, from the one table `help` prints (src/cli/cli.c).
const char *rp_cli_usage(const char *command);

// Prints "usage: rubrapack <the command's usage>" as a usage error; returns RP_EXIT_USAGE.
int rp_cli_usage_error(const char *command);

#endif // RUBRAPACK_CLI_H
