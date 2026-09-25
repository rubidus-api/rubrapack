#ifndef RUBRAPACK_PAL_H
#define RUBRAPACK_PAL_H

// include/rubrapack/pal.h
//
// Platform layer. The rest of rubrapack sees UTF-8 only; each PAL converts at its own
// boundary (RFC-0001 section 15.4):
//   Windows  wmain + UTF-16 argv -> UTF-8; a console gets WriteConsoleW (UTF-16), a pipe or
//            file gets the UTF-8 bytes unchanged.
//   POSIX    argv and output are UTF-8 bytes as they are.

#include <stddef.h>
#include <stdint.h>

#include "proven/types.h"

typedef enum {
    RP_OUT_STDOUT,
    RP_OUT_STDERR,
} rp_out_t;

// Writes UTF-8 text to stdout or stderr. The text must be valid UTF-8.
[[nodiscard]] proven_err_t rp_pal_write(rp_out_t out, const uint8_t *utf8, size_t len);

// Writes a NUL-terminated UTF-8 string.
[[nodiscard]] proven_err_t rp_pal_puts(rp_out_t out, const char *utf8);

#endif // RUBRAPACK_PAL_H
