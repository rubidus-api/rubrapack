// src/pal/posix/pal_posix.c - POSIX output: UTF-8 bytes as they are.

#include "rubrapack/pal.h"

#include <stdio.h>
#include <string.h>

proven_err_t rp_pal_write(rp_out_t out, const uint8_t *utf8, size_t len) {
    FILE *f = (out == RP_OUT_STDERR) ? stderr : stdout;
    if (utf8 == NULL && len != 0) return PROVEN_ERR_INVALID_ARG;
    if (len != 0 && fwrite(utf8, 1, len, f) != len) return PROVEN_ERR_IO;
    if (fflush(f) != 0) return PROVEN_ERR_IO;
    return PROVEN_OK;
}

proven_err_t rp_pal_puts(rp_out_t out, const char *utf8) {
    if (utf8 == NULL) return PROVEN_ERR_INVALID_ARG;
    return rp_pal_write(out, (const uint8_t *)utf8, strlen(utf8));
}
