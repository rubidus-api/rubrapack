#ifndef RUBRAPACK_DIAG_H
#define RUBRAPACK_DIAG_H

// include/rubrapack/diag.h
//
// Exit codes and one-line diagnostics (RFC-0001 section 7). Diagnostics go to stderr, results
// to stdout. Tool-level messages read `rubrapack: error[RP0001]: text`; source messages will
// read `file:line:col: error[RP1234]: text`.

// Process exit codes.
enum {
    RP_EXIT_OK = 0,
    RP_EXIT_SOURCE = 1,     // error in the .rpk source
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

// MinGW checks `printf` formats against the MS runtime, which rejects %zu; the UCRT that
// rubrapack links accepts C99 formats, so check against GNU rules there.
#if defined(__MINGW32__)
#define RP_PRINTF_FORMAT gnu_printf
#else
#define RP_PRINTF_FORMAT printf
#endif

// Prints `rubrapack: error[CODE]: message` to stderr. The formatted message must be UTF-8;
// it is cut at 1023 bytes on a character boundary.
[[gnu::format(RP_PRINTF_FORMAT, 2, 3)]]
void rp_diag_error(const char *code, const char *fmt, ...);
[[gnu::format(RP_PRINTF_FORMAT, 2, 3)]]
void rp_diag_warning(const char *code, const char *fmt, ...);

#endif // RUBRAPACK_DIAG_H
