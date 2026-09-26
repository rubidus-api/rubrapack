// src/pal/win32/pal_win32.c - Windows output: a console gets UTF-16 through WriteConsoleW,
// a pipe or file gets the UTF-8 bytes unchanged (RFC-0001 section 15.4).

#include "rubrapack/mem.h"
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

proven_err_t rp_pal_read_file(proven_allocator_t alloc, const char *path_utf8, size_t max_bytes,
                              uint8_t **data, size_t *len) {
    if (path_utf8 == NULL || data == NULL || len == NULL) return PROVEN_ERR_INVALID_ARG;
    proven_u16str_t wide = { 0 };
    proven_u8str_view_t view = { .ptr = (const proven_byte_t *)path_utf8, .size = strlen(path_utf8) };
    rp_text_result_t t = rp_utf8_to_u16str(alloc, view, &wide);
    if (t.err != PROVEN_OK) return t.err;
    HANDLE h = CreateFileW((const wchar_t *)proven_u16str_as_ptr(&wide), GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    proven_u16str_destroy(alloc, &wide);
    if (h == INVALID_HANDLE_VALUE) return PROVEN_ERR_NOT_FOUND;
    proven_err_t err = PROVEN_OK;
    LARGE_INTEGER size;
    uint8_t *buf = NULL;
    if (!GetFileSizeEx(h, &size) || size.QuadPart < 0) err = PROVEN_ERR_IO;
    else if ((unsigned long long)size.QuadPart > max_bytes) err = PROVEN_ERR_OUT_OF_BOUNDS;
    if (err == PROVEN_OK) {
        buf = rp_mem_alloc(alloc, (size_t)size.QuadPart, 1);
        if (buf == NULL) err = PROVEN_ERR_NOMEM;
    }
    for (size_t off = 0; err == PROVEN_OK && off < (size_t)size.QuadPart;) {
        size_t left = (size_t)size.QuadPart - off;
        DWORD want = left > 0x10000000u ? 0x10000000u : (DWORD)left;
        DWORD got = 0;
        if (!ReadFile(h, buf + off, want, &got, NULL) || got == 0) err = PROVEN_ERR_IO;
        off += got;
    }
    CloseHandle(h);
    if (err != PROVEN_OK) {
        rp_mem_free(alloc, buf);
        return err;
    }
    *data = buf;
    *len = (size_t)size.QuadPart;
    return PROVEN_OK;
}

static proven_err_t wide_path(proven_allocator_t alloc, const char *path_utf8, proven_u16str_t *out) {
    proven_u8str_view_t view = { .ptr = (const proven_byte_t *)path_utf8, .size = strlen(path_utf8) };
    rp_text_result_t t = rp_utf8_to_u16str(alloc, view, out);
    return t.err;
}

rp_fskind_t rp_pal_stat(proven_allocator_t alloc, const char *path_utf8, uint64_t *size) {
    proven_u16str_t w = { 0 };
    if (path_utf8 == NULL || wide_path(alloc, path_utf8, &w) != PROVEN_OK) return RP_FS_NONE;
    WIN32_FILE_ATTRIBUTE_DATA a;
    BOOL ok = GetFileAttributesExW((const wchar_t *)proven_u16str_as_ptr(&w), GetFileExInfoStandard, &a);
    proven_u16str_destroy(alloc, &w);
    if (!ok) return RP_FS_NONE;
    if (a.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) return RP_FS_LINK;
    if (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return RP_FS_DIR;
    if (size) *size = ((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow;
    return RP_FS_FILE;
}

proven_err_t rp_pal_write_file_atomic(proven_allocator_t alloc, const char *path_utf8, const uint8_t *data, size_t len) {
    if (path_utf8 == NULL || (data == NULL && len != 0)) return PROVEN_ERR_INVALID_ARG;
    proven_u16str_t w = { 0 }, t = { 0 };
    size_t n = strlen(path_utf8);
    char *tmp = rp_mem_alloc(alloc, n + 16, 1);
    if (tmp == NULL) return PROVEN_ERR_NOMEM;
    memcpy(tmp, path_utf8, n);
    memcpy(tmp + n, ".rp-tmp", 8);
    proven_err_t err = wide_path(alloc, path_utf8, &w);
    if (err == PROVEN_OK) err = wide_path(alloc, tmp, &t);
    rp_mem_free(alloc, tmp);
    if (err != PROVEN_OK) {
        proven_u16str_destroy(alloc, &w);
        return err;
    }
    const wchar_t *wt = (const wchar_t *)proven_u16str_as_ptr(&t);
    HANDLE h = CreateFileW(wt, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        err = PROVEN_ERR_IO;
    } else {
        for (size_t off = 0; err == PROVEN_OK && off < len;) {
            DWORD chunk = (len - off) > 0x10000000u ? 0x10000000u : (DWORD)(len - off), done = 0;
            if (!WriteFile(h, data + off, chunk, &done, NULL) || done == 0) err = PROVEN_ERR_IO;
            off += done;
        }
        if (!FlushFileBuffers(h)) err = PROVEN_ERR_IO;
        CloseHandle(h);
        if (err == PROVEN_OK &&
            !MoveFileExW(wt, (const wchar_t *)proven_u16str_as_ptr(&w), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            err = PROVEN_ERR_IO;
        }
        if (err != PROVEN_OK) DeleteFileW(wt);
    }
    proven_u16str_destroy(alloc, &w);
    proven_u16str_destroy(alloc, &t);
    return err;
}

proven_err_t rp_pal_list_dir(proven_allocator_t alloc, const char *path_utf8, char ***names, size_t *count) {
    if (path_utf8 == NULL || names == NULL || count == NULL) return PROVEN_ERR_INVALID_ARG;
    size_t pl = strlen(path_utf8);
    char *pattern = rp_mem_alloc(alloc, pl + 3, 1);
    if (pattern == NULL) return PROVEN_ERR_NOMEM;
    memcpy(pattern, path_utf8, pl);
    memcpy(pattern + pl, "\\*", 3);
    proven_u16str_t w = { 0 };
    proven_err_t err = wide_path(alloc, pattern, &w);
    rp_mem_free(alloc, pattern);
    if (err != PROVEN_OK) return err;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((const wchar_t *)proven_u16str_as_ptr(&w), &fd);
    proven_u16str_destroy(alloc, &w);
    if (h == INVALID_HANDLE_VALUE) return PROVEN_ERR_NOT_FOUND;
    char **v = NULL;
    size_t n = 0, cap = 0;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        proven_u8str_t u = { 0 };
        proven_u16str_view_t view = { .ptr = (const proven_u16 *)fd.cFileName, .size = wcslen(fd.cFileName) };
        rp_text_result_t r = rp_utf16_to_u8str(alloc, view, &u);
        if (r.err != PROVEN_OK) {
            err = r.err;
            break;
        }
        if (n == cap) {
            size_t ncap = cap ? cap * 2 : 32;
            char **nv = rp_mem_alloc(alloc, ncap, sizeof *nv);
            if (nv == NULL) {
                proven_u8str_destroy(alloc, &u);
                err = PROVEN_ERR_NOMEM;
                break;
            }
            if (n) memcpy(nv, v, n * sizeof *nv);
            rp_mem_free(alloc, v);
            v = nv;
            cap = ncap;
        }
        size_t len = strlen(proven_u8str_as_cstr(&u));
        v[n] = rp_mem_alloc(alloc, len + 1, 1);
        if (v[n]) memcpy(v[n++], proven_u8str_as_cstr(&u), len + 1);
        else err = PROVEN_ERR_NOMEM;
        proven_u8str_destroy(alloc, &u);
    } while (err == PROVEN_OK && FindNextFileW(h, &fd));
    FindClose(h);
    if (err != PROVEN_OK) {
        for (size_t k = 0; k < n; ++k) rp_mem_free(alloc, v[k]);
        rp_mem_free(alloc, v);
        return err;
    }
    *names = v;
    *count = n;
    return PROVEN_OK;
}

static bool file_id(proven_allocator_t alloc, const char *path_utf8, BY_HANDLE_FILE_INFORMATION *info) {
    proven_u16str_t w = { 0 };
    if (wide_path(alloc, path_utf8, &w) != PROVEN_OK) return false;
    HANDLE h = CreateFileW((const wchar_t *)proven_u16str_as_ptr(&w), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    proven_u16str_destroy(alloc, &w);
    if (h == INVALID_HANDLE_VALUE) return false;
    BOOL ok = GetFileInformationByHandle(h, info);
    CloseHandle(h);
    return ok != 0;
}

bool rp_pal_same_file(proven_allocator_t alloc, const char *a_utf8, const char *b_utf8) {
    BY_HANDLE_FILE_INFORMATION a, b;
    if (a_utf8 == NULL || b_utf8 == NULL || !file_id(alloc, a_utf8, &a) || !file_id(alloc, b_utf8, &b)) return false;
    return a.dwVolumeSerialNumber == b.dwVolumeSerialNumber && a.nFileIndexHigh == b.nFileIndexHigh &&
           a.nFileIndexLow == b.nFileIndexLow;
}
