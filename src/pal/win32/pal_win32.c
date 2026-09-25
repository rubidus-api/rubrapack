// src/pal/win32/pal_win32.c - Windows output: a console gets UTF-16 through WriteConsoleW,
// a pipe or file gets the UTF-8 bytes unchanged (RFC-0001 section 15.4).

#include "rubrapack/pal.h"
#include "rubrapack/text.h"

#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static proven_err_t write_bytes(HANDLE h, const uint8_t *p, size_t len) {
    while (len > 0) {
        DWORD chunk = (len > 0x10000u) ? 0x10000u : (DWORD)len;
        DWORD done = 0;
        if (!WriteFile(h, p, chunk, &done, NULL) || done == 0) return PROVEN_ERR_IO;
        p += done;
        len -= done;
    }
    return PROVEN_OK;
}

// Converts and writes in pieces that end on a UTF-8 sequence boundary, so no buffer larger
// than `wide` is needed and a surrogate pair is never split between two calls.
static proven_err_t write_console(HANDLE h, const uint8_t *p, size_t len) {
    enum { PIECE = 1024 };
    wchar_t wide[PIECE * 2];
    while (len > 0) {
        size_t take = (len > PIECE) ? PIECE : len;
        while (take < len && take > 0 && (p[take] & 0xC0u) == 0x80u) --take;
        if (take == 0) return PROVEN_ERR_INVALID_ENCODING;
        rp_text_result_t r = rp_utf8_to_utf16(p, take, (proven_u16 *)wide, PIECE * 2);
        if (r.err != PROVEN_OK) return r.err;
        DWORD left = (DWORD)r.units;
        const wchar_t *w = wide;
        while (left > 0) {
            DWORD done = 0;
            if (!WriteConsoleW(h, w, left, &done, NULL) || done == 0) return PROVEN_ERR_IO;
            w += done;
            left -= done;
        }
        p += take;
        len -= take;
    }
    return PROVEN_OK;
}

proven_err_t rp_pal_write(rp_out_t out, const uint8_t *utf8, size_t len) {
    if (utf8 == NULL && len != 0) return PROVEN_ERR_INVALID_ARG;
    HANDLE h = GetStdHandle(out == RP_OUT_STDERR ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
    if (h == INVALID_HANDLE_VALUE || h == NULL) return PROVEN_ERR_IO;
    DWORD mode;
    if (GetConsoleMode(h, &mode)) return write_console(h, utf8, len);
    return write_bytes(h, utf8, len);
}

proven_err_t rp_pal_puts(rp_out_t out, const char *utf8) {
    if (utf8 == NULL) return PROVEN_ERR_INVALID_ARG;
    return rp_pal_write(out, (const uint8_t *)utf8, strlen(utf8));
}
