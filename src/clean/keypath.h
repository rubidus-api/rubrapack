// src/clean/keypath.h - the folder an installed component's key path (MsiGetComponentPath) stands
// for, shared by the helper DLL (src/ca: the purge guard, the removal preflight) and the cleanup
// program (src/clean: the files installed products own). Included by both.
//
// A key path is a file ("C:\...\a.dll": its folder), a folder ("C:\...\": itself) or a registry
// value ("NN:\key\name", observed "21:\Software\...\Components\C_..."): the first digit 0 for the
// 32-bit view, 2 for the 64-bit one; the second the root, 0 HKCR, 1 HKCU, 2 HKLM, 3 HKU. A registry
// key path names no folder - unless its value is one: rubrapack's profile components (RFC-0029)
// hold their folder there since 2026-11-10 (0.47.0-0.52.0 wrote 1). A string value of another
// vendor that is an absolute folder counts too: a folder is then kept, never deleted.
//
// The parsing has no Windows calls (tests/test_keypath.c runs it on Linux); keypath_dir, which
// reads the registry, is for Windows only.

#ifndef RUBRAPACK_KEYPATH_H
#define RUBRAPACK_KEYPATH_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "../ca/rp_wstr.h"

// "X:\..." - an absolute path on a drive.
static inline bool keypath_is_drive_path(const wchar_t *p) {
    return ((p[0] >= L'A' && p[0] <= L'Z') || (p[0] >= L'a' && p[0] <= L'z')) && p[1] == L':' && p[2] == L'\\';
}

// A file or folder key path: its folder (ending in '\') into out. False for anything else.
static inline bool keypath_file_dir(const wchar_t *kp, wchar_t *out, size_t cap) {
    if (!keypath_is_drive_path(kp) || !rp_wcpy(out, cap, kp)) return false;
    wchar_t *slash = rp_wrchr(out, L'\\');
    slash[1] = 0;                          // a folder's own '\' or the one before the file name
    return true;
}

// A registry key path: its root (0-3), view (true: 64-bit), key (into key) and value name (a
// pointer into kp; empty for the default value). False for anything else.
static inline bool keypath_reg_split(const wchar_t *kp, int *root, bool *wow64, wchar_t *key, size_t cap, const wchar_t **name) {
    if (!(kp[0] == L'0' || kp[0] == L'2') || kp[1] < L'0' || kp[1] > L'3' || kp[2] != L':' || kp[3] != L'\\') return false;
    const wchar_t *rest = kp + 4, *last = rp_wrchr(rest, L'\\');
    if (last == NULL || last == rest) return false;
    size_t n = (size_t)(last - rest);
    if (n >= cap) return false;
    memcpy(key, rest, n * sizeof *key);
    key[n] = 0;
    *root = kp[1] - L'0';
    *wow64 = kp[0] == L'2';
    *name = last + 1;
    return true;
}

// A registry value that is an absolute folder: the folder, ending in '\', into out.
static inline bool keypath_value_dir(const wchar_t *v, wchar_t *out, size_t cap) {
    size_t n = rp_wlen(v);
    if (!keypath_is_drive_path(v) || rp_wstr(v, L"..") != NULL || n + 2 > cap) return false;
    memcpy(out, v, (n + 1) * sizeof *out);
    if (out[n - 1] != L'\\') {
        out[n] = L'\\';
        out[n + 1] = 0;
    }
    return true;
}

// How many characters of `path` to compare with folder `root` to tell whether `path` is inside it
// (not the root itself): `root` without its trailing '\', when `path` is longer and has a '\' and
// more after that many characters; 0 otherwise (also for an empty root). The caller compares those
// characters (CompareStringOrdinal, ignoring case): never past the end of the shorter string.
static inline size_t keypath_inside_n(const wchar_t *path, const wchar_t *root) {
    size_t n = rp_wlen(root);
    while (n && root[n - 1] == L'\\') --n;
    return n && rp_wlen(path) > n + 1 && path[n] == L'\\' ? n : 0;
}

// Whether `path` has the form of one of the installer's backups, <volume>\Config.Msi\<name>.rbf, one
// level down - except the case of "Config.Msi" and ".rbf", which the caller compares: it returns
// the offset of the name (14), or 0.
static inline size_t keypath_backup_name(const wchar_t *path) {
    size_t n = rp_wlen(path);
    if (n < 14 + 5 || path[1] != L':' || path[2] != L'\\' || path[13] != L'\\') return 0;
    return rp_wchr(path + 14, L'\\') ? 0 : 14;
}

#if defined(_WIN32)
#include <windows.h>

// keypath_inside_n with the comparison, ignoring case as Windows does.
[[maybe_unused]] static bool keypath_inside(const wchar_t *path, const wchar_t *root) {
    size_t n = keypath_inside_n(path, root);
    return n && CompareStringOrdinal(path, (int)n, root, (int)n, TRUE) == CSTR_EQUAL;
}

// <volume>\Config.Msi\<name>.rbf, one level down: one of the installer's backups.
[[maybe_unused]] static bool keypath_is_backup(const wchar_t *path) {
    size_t at = keypath_backup_name(path), n = rp_wlen(path);
    return at && CompareStringOrdinal(path + 3, 10, L"Config.Msi", 10, TRUE) == CSTR_EQUAL &&
           CompareStringOrdinal(path + n - 4, 4, L".rbf", 4, TRUE) == CSTR_EQUAL;
}

// The folder of key path `kp` (ending in '\'), reading a registry value as the current user.
[[maybe_unused]] static bool keypath_dir(const wchar_t *kp, wchar_t *out, size_t cap) {
    if (keypath_file_dir(kp, out, cap)) return true;
    static wchar_t key[1024], value[1024];
    int root;
    bool wow64;
    const wchar_t *name;
    if (!keypath_reg_split(kp, &root, &wow64, key, sizeof key / sizeof *key, &name)) return false;
    static const HKEY roots[4] = { HKEY_CLASSES_ROOT, HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE, HKEY_USERS };
    HKEY base = roots[root], cu = NULL, k = NULL;
    if (root == 1 && RegOpenCurrentUser(KEY_READ, &cu) == ERROR_SUCCESS) base = cu;     // the impersonated user's
    bool ok = false;
    if (RegOpenKeyExW(base, key, 0, KEY_QUERY_VALUE | (wow64 ? KEY_WOW64_64KEY : KEY_WOW64_32KEY), &k) == ERROR_SUCCESS) {
        DWORD type = 0, size = sizeof value - sizeof *value;
        if (RegQueryValueExW(k, name, NULL, &type, (BYTE *)value, &size) == ERROR_SUCCESS && type == REG_SZ) {
            value[size / sizeof *value] = 0;
            ok = keypath_value_dir(value, out, cap);
        }
        RegCloseKey(k);
    }
    if (cu) RegCloseKey(cu);
    return ok;
}
#endif

#endif
