// src/core/diag.c - one-line diagnostics on stderr (include/rubrapack/diag.h).

#include "rubrapack/diag.h"
#include "rubrapack/pal.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static void emit(const char *kind, const char *code, const char *fmt, va_list ap) {
    char msg[1024];
    int n = vsnprintf(msg, sizeof msg, fmt, ap);
    if (n < 0) {
        msg[0] = '\0';
    } else if ((size_t)n >= sizeof msg) {
        // vsnprintf cut the text at a byte. If the last character lost bytes, drop it whole.
        size_t len = sizeof msg - 1;
        size_t p = len;
        while (p > 0 && ((unsigned char)msg[p - 1] & 0xC0u) == 0x80u) --p;
        if (p > 0) {
            unsigned char lead = (unsigned char)msg[p - 1];
            size_t need = lead < 0x80 ? 1 : lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : 2;
            if ((p - 1) + need > len) msg[p - 1] = '\0';
        }
    }

    char line[1100];
    int m = snprintf(line, sizeof line, "rubrapack: %s[%s]: %s\n", kind, code, msg);
    if (m < 0) return;
    size_t len = strlen(line);
    (void)rp_pal_write(RP_OUT_STDERR, (const uint8_t *)line, len);
}

void rp_diag_error(const char *code, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    emit("error", code, fmt, ap);
    va_end(ap);
}

void rp_diag_warning(const char *code, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    emit("warning", code, fmt, ap);
    va_end(ap);
}
