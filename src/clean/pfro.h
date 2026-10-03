// src/clean/pfro.h - Windows' list of renames for the next restart (PendingFileRenameOperations),
// shared by the helper DLL (src/ca) and the cleanup program (src/clean). Included by both.
//
// The value is pairs of source and target, each NUL-terminated, the list ended by an empty string;
// an empty target is a deletion. Windows 11 writes a source as "*1\??\C:\..." (a flag, then the NT
// prefix; observed), others write "\??\C:\..."; "!" marks a replacing rename.
//
// A file a running program holds is queued for deletion when its package is removed. If another
// installation then installs an identical file at that path, Windows Installer keeps the file
// already there ("Won't Overwrite ... hash matches", observed x41) and the old deletion stays:
// the restart would delete the newly installed file. pfro_cancel takes such a deletion out.

#ifndef RUBRAPACK_PFRO_H
#define RUBRAPACK_PFRO_H

#include <stdbool.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

#define PFRO_KEY L"SYSTEM\\CurrentControlSet\\Control\\Session Manager"
#define PFRO_VALUE L"PendingFileRenameOperations"

[[maybe_unused]] static const wchar_t *pfro_path(const wchar_t *src) {
    if (*src == L'*') {
        ++src;
        while (*src >= L'0' && *src <= L'9') ++src;
    }
    if (*src == L'!') ++src;
    return wcsncmp(src, L"\\??\\", 4) == 0 ? src + 4 : src;
}

// The value (heap, two extra NULs) and its size in bytes; NULL when there is none.
[[maybe_unused]] static wchar_t *pfro_read(HKEY k, DWORD *size) {
    DWORD type = 0;
    *size = 0;
    if (RegQueryValueExW(k, PFRO_VALUE, NULL, &type, NULL, size) != ERROR_SUCCESS || type != REG_MULTI_SZ || *size == 0) return NULL;
    wchar_t *buf = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, *size + 2 * sizeof(wchar_t));
    if (buf && RegQueryValueExW(k, PFRO_VALUE, NULL, &type, (BYTE *)buf, size) != ERROR_SUCCESS) {
        HeapFree(GetProcessHeap(), 0, buf);
        buf = NULL;
    }
    return buf;
}

// Whether a deletion of `path` is queued (any position).
[[maybe_unused]] static bool pfro_has(const wchar_t *path) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, PFRO_KEY, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS) return false;
    DWORD size = 0;
    wchar_t *buf = pfro_read(k, &size);
    RegCloseKey(k);
    bool found = false;
    for (wchar_t *src = buf; src && *src && !found;) {
        wchar_t *dst = src + wcslen(src) + 1;
        found = *dst == 0 && CompareStringOrdinal(pfro_path(src), -1, path, -1, TRUE) == CSTR_EQUAL;
        src = *dst ? dst + wcslen(dst) + 1 : dst + 1;
    }
    if (buf) HeapFree(GetProcessHeap(), 0, buf);
    return found;
}

// Takes the queued deletions of `path` among the first `before` pairs out of the list (all of them
// with before = (unsigned long)-1). Needs write access to HKLM (SYSTEM or an administrator).
// Returns how many it took out, or -1 when the list could not be read or written.
[[maybe_unused]] static int pfro_cancel(const wchar_t *path, unsigned long before) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, PFRO_KEY, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &k) != ERROR_SUCCESS) return -1;
    DWORD size = 0;
    wchar_t *buf = pfro_read(k, &size);
    int taken = 0;
    if (buf) {
        wchar_t *out = buf;          // compacts in place: the kept pairs move down
        unsigned long index = 0;
        for (wchar_t *src = buf; *src; ++index) {
            wchar_t *dst = src + wcslen(src) + 1;
            wchar_t *next = *dst ? dst + wcslen(dst) + 1 : dst + 1;
            bool drop = *dst == 0 && index < before && CompareStringOrdinal(pfro_path(src), -1, path, -1, TRUE) == CSTR_EQUAL;
            if (drop) {
                ++taken;
            } else {
                size_t n = (size_t)(next - src);
                memmove(out, src, n * sizeof *out);
                out += n;
            }
            src = next;
        }
        *out++ = 0;
        if (taken) {
            LSTATUS ls = out - buf <= 1 ? RegDeleteValueW(k, PFRO_VALUE)
                                        : RegSetValueExW(k, PFRO_VALUE, 0, REG_MULTI_SZ, (const BYTE *)buf, (DWORD)((out - buf) * sizeof *buf));
            if (ls != ERROR_SUCCESS) taken = -1;
        }
        HeapFree(GetProcessHeap(), 0, buf);
    }
    RegCloseKey(k);
    return taken;
}

#endif
