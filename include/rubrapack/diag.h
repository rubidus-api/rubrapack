#ifndef RUBRAPACK_DIAG_H
#define RUBRAPACK_DIAG_H

// include/rubrapack/diag.h
//
// Exit codes and one-line diagnostics (RFC-0001 section 7). Diagnostics go to stderr, results
// to stdout. Tool-level messages read `rubrapack: error[RP0001]: text`; source messages will
// read `file:line:col: error[RP1234]: text`.

#include <stddef.h>

#include "proven/fmt.h"

// Process exit codes.
enum {
    RP_EXIT_OK = 0,
    RP_EXIT_SOURCE = 1,     // error in the .toml source
    RP_EXIT_USAGE = 2,      // bad command line, unknown or not yet implemented command
    RP_EXIT_IO = 3,         // input/output, including out of memory
    RP_EXIT_SIGN = 4,       // signing or verification failed
    RP_EXIT_LINT = 5,       // lint found errors
    RP_EXIT_NET = 6,        // network (timestamp server)
};

// Diagnostic codes. RP0xxx is the command line and the tool itself.
#define RP_DIAG_UNKNOWN_COMMAND "RP0001"
#define RP_DIAG_NOT_IMPLEMENTED "RP0002"
#define RP_DIAG_BAD_ARG_TEXT    "RP0003"
#define RP_DIAG_EXTRA_ARGUMENT  "RP0004"
#define RP_DIAG_NOMEM           "RP0005"
#define RP_DIAG_OUTPUT          "RP0006"
#define RP_DIAG_INPUT           "RP0007"
#define RP_DIAG_BAD_PACKAGE     "RP0008"
#define RP_DIAG_NO_TABLE        "RP0009"
#define RP_DIAG_INTERNAL        "RP0010"   // a check of rubrapack's own output failed: a bug to report
#define RP_DIAG_SIGN            "RP0011"   // signing could not be done (key, certificate, password, file)
#define RP_DIAG_VERIFY          "RP0012"   // a signature does not verify or is not trusted
#define RP_DIAG_TRANSFORM       "RP0013"   // a difference a transform cannot carry

// The arguments of a proven format, as proven_fmt_to passes them: the array (its first element
// empty) and its length. The formatting entry points below are macros over it.
#define RP_ARGS(...)                                                                                \
    ((const proven_arg_t[]){ proven_arg_none() __VA_OPT__(,) __VA_ARGS__ }),                       \
    (sizeof((const proven_arg_t[]){ proven_arg_none() __VA_OPT__(,) __VA_ARGS__ }) / sizeof(proven_arg_t))

// Prints `rubrapack: error[CODE]: message` to stderr: a proven format (`{}` for each value, the
// English text and the key of its translation) and its proven_arg_* values, as proven_fmt_to takes
// them. The message must be UTF-8; it is cut at 1023 bytes on a character boundary.
#define rp_diag_error(code, fmt, ...)   rp_diag_error_a(code, fmt, RP_ARGS(__VA_ARGS__))
#define rp_diag_warning(code, fmt, ...) rp_diag_warning_a(code, fmt, RP_ARGS(__VA_ARGS__))
void rp_diag_error_a(const char *code, const char *fmt, const proven_arg_t *args, size_t nargs);
void rp_diag_warning_a(const char *code, const char *fmt, const proven_arg_t *args, size_t nargs);

// `lint --json` (RFC-0025): from begin to end the diagnostics (these and the source ones of
// srcdiag.h) are collected instead of printed; end prints one JSON object on stdout:
// {"file", "exit", "errors", "warnings", "not_shown", "diagnostics": [{"file", "line", "column",
// "severity", "code", "message"}]} (line and column are 0 for one about the file as a whole).
void rp_diag_json_begin(void);
bool rp_diag_json_on(void);
void rp_diag_json_add(const char *path, unsigned line, unsigned col, const char *code, bool warning, const char *msg);
void rp_diag_json_not_shown(size_t n);
void rp_diag_json_end(const char *path, int rc);

#endif // RUBRAPACK_DIAG_H
