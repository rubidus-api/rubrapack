// src/clean/rubrapack_clean.c - the cleanup task's program (RFC-0026).
//
//   rubrapack_clean.exe <folder>
//
// The helper DLL's commit action puts this program and <folder>\list.txt in a folder of its own
// (%ProgramData%\rubrapack\cleanup\<ProductCode> for a per-machine package, the user's
// %LOCALAPPDATA%\rubrapack\cleanup\<ProductCode> for a per-user one) and registers a scheduled
// task that runs it at logon and every 15 minutes. Each run deletes what it can of:
//   - the deletions Windows has queued for the next restart (PendingFileRenameOperations) under
//     one of the list's roots (the package's folders), and, for a per-machine package, the
//     installer's backup copies (<volume>\Config.Msi\*.rbf) this installation queued (those
//     after the commit's count; others belong to other installations and are left alone);
//   - the files the list names (`file` lines: what a per-user package could not queue);
//   - each root that is left empty, once the product is no longer installed - only a root that
//     held a queued deletion (or a folder of the package above one): the folders Windows
//     Installer had to leave. A folder that was there before the installation stays.
// When nothing is left, or the list's time is up, it deletes its task and its folder. It never
// runs while an installation is running, never deletes outside the roots, Config.Msi and its own
// list, and never deletes through a link. A file still held is simply tried again next time; the
// restart still deletes it if the task gave up.
//
// list.txt, UTF-16LE: "RPC1", then one "<key>\t<value>" per line: task (the task's name), until
// (the last day, YYYYMMDD, local time), product ({ProductCode}), scope (machine or user), after
// (how many queued renames there were at the commit), root (a folder), file (a file).

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>
#include <msi.h>
#include <shellapi.h>

enum { MAX_ITEMS = 256, PATH_CAP = 32768 };

static wchar_t task[512], product[64], scope[16], until[16];
static wchar_t *roots[MAX_ITEMS], *files[MAX_ITEMS];
static bool seen[MAX_ITEMS];        // a root that held a queued deletion (seen.txt keeps it between runs)
static size_t nroots, nfiles;
static unsigned long after;         // queued renames at the commit (list.txt "after")

static wchar_t *text_dup(const wchar_t *s) {
    size_t n = wcslen(s) + 1;
    wchar_t *d = HeapAlloc(GetProcessHeap(), 0, n * sizeof *d);
    if (d) memcpy(d, s, n * sizeof *d);
    return d;
}

static bool read_list(const wchar_t *path) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD size = GetFileSize(h, NULL), got = 0;
    if (size == INVALID_FILE_SIZE || size > 4u << 20 || size % 2) {
        CloseHandle(h);
        return false;
    }
    wchar_t *text = HeapAlloc(GetProcessHeap(), 0, size + sizeof(wchar_t));
    bool ok = text && ReadFile(h, text, size, &got, NULL) && got == size;
    CloseHandle(h);
    if (!ok) return false;
    text[size / 2] = 0;
    wchar_t *p = text;
    if (*p == 0xFEFF) ++p;
    if (wcsncmp(p, L"RPC1", 4) != 0) return false;
    wchar_t *ctx = NULL;
    for (wchar_t *line = wcstok(p, L"\r\n", &ctx); line; line = wcstok(NULL, L"\r\n", &ctx)) {
        wchar_t *tab = wcschr(line, L'\t');
        if (tab == NULL) continue;
        *tab = 0;
        const wchar_t *v = tab + 1;
        if (wcscmp(line, L"task") == 0) wcsncpy(task, v, 511);
        else if (wcscmp(line, L"product") == 0) wcsncpy(product, v, 63);
        else if (wcscmp(line, L"scope") == 0) wcsncpy(scope, v, 15);
        else if (wcscmp(line, L"until") == 0) wcsncpy(until, v, 15);
        else if (wcscmp(line, L"after") == 0) after = wcstoul(v, NULL, 10);
        else if (wcscmp(line, L"root") == 0 && nroots < MAX_ITEMS) roots[nroots++] = text_dup(v);
        else if (wcscmp(line, L"file") == 0 && nfiles < MAX_ITEMS) files[nfiles++] = text_dup(v);
    }
    return task[0] != 0;
}

// Whether `path` is inside folder `root` (not the root itself), ignoring case.
static bool under(const wchar_t *path, const wchar_t *root) {
    size_t n = wcslen(root);
    while (n && root[n - 1] == L'\\') --n;
    return n && CompareStringOrdinal(path, (int)n, root, (int)n, TRUE) == CSTR_EQUAL && path[n] == L'\\' && path[n + 1];
}

// <volume>\Config.Msi\<name>.rbf, one level down.
static bool installer_backup(const wchar_t *path) {
    const wchar_t *dir = path + 2;
    if (!(path[0] && path[1] == L':')) return false;
    if (CompareStringOrdinal(dir, 12, L"\\Config.Msi\\", 12, TRUE) != CSTR_EQUAL) return false;
    const wchar_t *name = dir + 12;
    size_t n = wcslen(name);
    return n > 4 && !wcschr(name, L'\\') && CompareStringOrdinal(name + n - 4, 4, L".rbf", 4, TRUE) == CSTR_EQUAL;
}

// No part of `path` below `root` (or below the volume, without a root) is a link.
static bool no_links(const wchar_t *path, const wchar_t *root) {
    static wchar_t part[PATH_CAP];
    size_t start = root ? wcslen(root) : 3, n = wcslen(path);
    if (n >= PATH_CAP) return false;
    for (size_t i = start; i <= n; ++i) {
        if (i < n && path[i] != L'\\') continue;
        memcpy(part, path, i * sizeof *part);
        part[i] = 0;
        DWORD a = GetFileAttributesW(part);
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
    }
    return true;
}

// Tries to delete a file or an empty folder; true when it is gone (or was not there).
static bool gone(const wchar_t *path) {
    DWORD a = GetFileAttributesW(path);
    if (a == INVALID_FILE_ATTRIBUTES) return true;
    if (a & FILE_ATTRIBUTE_DIRECTORY) return RemoveDirectoryW(path) != 0;
    if (a & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(path, a & ~FILE_ATTRIBUTE_READONLY);
    return DeleteFileW(path) != 0;
}

// A PendingFileRenameOperations source as a path: Windows writes "*1\??\C:\..." (a flag, then the
// NT prefix; seen on Windows 11) or "\??\C:\..."; "!" marks a replacing rename.
static const wchar_t *nt_path(const wchar_t *src) {
    if (*src == L'*') {
        ++src;
        while (*src >= L'0' && *src <= L'9') ++src;
    }
    if (*src == L'!') ++src;
    return wcsncmp(src, L"\\??\\", 4) == 0 ? src + 4 : src;
}

// The installer's backups (<volume>\Config.Msi\*.rbf) this installation queued: found once, on the
// first run, among the queued deletions after the first `after` of them (the commit counted those
// that were there before; Windows Installer adds its own after the commit). Kept in seen.txt.
static wchar_t *backups[MAX_ITEMS];
static size_t nbackups;
static bool scanned;

// The queued deletions under the roots: tried; returns how many are still there afterwards.
static int pending(bool machine) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager", 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS)
        return 0;
    DWORD type = 0, size = 0;
    int left = 0;
    if (RegQueryValueExW(k, L"PendingFileRenameOperations", NULL, &type, NULL, &size) == ERROR_SUCCESS && type == REG_MULTI_SZ && size) {
        wchar_t *buf = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size + 2 * sizeof(wchar_t));
        if (buf && RegQueryValueExW(k, L"PendingFileRenameOperations", NULL, &type, (BYTE *)buf, &size) == ERROR_SUCCESS) {
            // Pairs of source and target; an empty target is a deletion.
            unsigned long index = 0;
            for (wchar_t *src = buf; *src; ++index) {
                wchar_t *dst = src + wcslen(src) + 1;
                const wchar_t *path = nt_path(src);
                if (*dst == 0) {
                    const wchar_t *root = NULL;
                    for (size_t i = 0; i < nroots; ++i) {
                        if (!under(path, roots[i])) continue;
                        seen[i] = true;
                        if (root == NULL || wcslen(roots[i]) > wcslen(root)) root = roots[i];
                    }
                    if (root && no_links(path, root) && !gone(path)) ++left;
                    if (machine && !scanned && index >= after && installer_backup(path) && nbackups < MAX_ITEMS) backups[nbackups++] = text_dup(path);
                }
                src = *dst ? dst + wcslen(dst) + 1 : dst + 1;
            }
        }
        if (buf) HeapFree(GetProcessHeap(), 0, buf);
    }
    RegCloseKey(k);
    scanned = true;
    for (size_t i = 0; i < nbackups; ++i) {
        if (no_links(backups[i], NULL) && !gone(backups[i])) ++left;
    }
    return left;
}

// seen.txt (UTF-16LE): "scanned", then "root<TAB>path" for each root that held a queued deletion
// and "backup<TAB>path" for each backup found on the first run.
static void load_seen(const wchar_t *path) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD size = GetFileSize(h, NULL), got = 0;
    wchar_t *t = size != INVALID_FILE_SIZE && size < (1u << 20) && size % 2 == 0 ? HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size + 2) : NULL;
    if (t && ReadFile(h, t, size, &got, NULL) && got == size) {
        wchar_t *ctx = NULL;
        for (wchar_t *line = wcstok(t, L"\r\n", &ctx); line; line = wcstok(NULL, L"\r\n", &ctx)) {
            if (wcscmp(line, L"scanned") == 0) scanned = true;
            if (wcsncmp(line, L"backup\t", 7) == 0 && nbackups < MAX_ITEMS) backups[nbackups++] = text_dup(line + 7);
            if (wcsncmp(line, L"root\t", 5) != 0) continue;
            for (size_t i = 0; i < nroots; ++i) {
                if (CompareStringOrdinal(line + 5, -1, roots[i], -1, TRUE) == CSTR_EQUAL) seen[i] = true;
            }
        }
    }
    CloseHandle(h);
}

static void put_line(HANDLE h, const wchar_t *a, const wchar_t *b) {
    DWORD put = 0;
    WriteFile(h, a, (DWORD)(wcslen(a) * sizeof(wchar_t)), &put, NULL);
    if (b) WriteFile(h, b, (DWORD)(wcslen(b) * sizeof(wchar_t)), &put, NULL);
    WriteFile(h, L"\r\n", 2 * sizeof(wchar_t), &put, NULL);
}

static void save_seen(const wchar_t *path) {
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    if (scanned) put_line(h, L"scanned", NULL);
    for (size_t i = 0; i < nroots; ++i) {
        if (seen[i]) put_line(h, L"root\t", roots[i]);
    }
    for (size_t i = 0; i < nbackups; ++i) put_line(h, L"backup\t", backups[i]);
    CloseHandle(h);
}

static bool empty_dir(const wchar_t *dir) {
    static wchar_t pat[PATH_CAP];
    swprintf(pat, PATH_CAP, L"%ls\\*", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(pat, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool empty = true;
    do {
        if (wcscmp(fd.cFileName, L".") != 0 && wcscmp(fd.cFileName, L"..") != 0) empty = false;
    } while (empty && FindNextFileW(h, &fd));
    FindClose(h);
    return empty;
}

// Deletes the task, then the folder with this program in it (a moment later, from cmd).
static void finish(const wchar_t *folder) {
    static wchar_t cmd[PATH_CAP], sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    STARTUPINFOW si = { .cb = sizeof si };
    PROCESS_INFORMATION pi;
    swprintf(cmd, PATH_CAP, L"\"%ls\\schtasks.exe\" /delete /tn \"%ls\" /f", sys, task);
    if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 30000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    // The parent folders go too when this was the last one (rd leaves a folder that is not empty).
    swprintf(cmd, PATH_CAP, L"\"%ls\\cmd.exe\" /c ping -n 3 127.0.0.1 >nul & rd /s /q \"%ls\" & rd \"%ls\\..\" 2>nul & rd \"%ls\\..\\..\" 2>nul",
             sys, folder, folder, folder);
    if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
}

// A windows program, so a per-user task shows no console window every 15 minutes.
int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR line, int show) {
    (void)inst;
    (void)prev;
    (void)show;
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(line && line[0] ? GetCommandLineW() : L"x", &argc);
    if (argv == NULL || argc != 2) return 2;
    static wchar_t folder[PATH_CAP], list[PATH_CAP];
    if (GetFullPathNameW(argv[1], PATH_CAP, folder, NULL) == 0) return 2;
    size_t fl = wcslen(folder);
    while (fl > 3 && folder[fl - 1] == L'\\') folder[--fl] = 0;
    swprintf(list, PATH_CAP, L"%ls\\list.txt", folder);
    if (!read_list(list)) return 3;
    // Not while Windows Installer runs an installation (its backups may still be needed).
    HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, L"Global\\_MSIExecute");
    if (m) {
        CloseHandle(m);
        return 0;
    }
    bool machine = wcscmp(scope, L"machine") == 0;
    // The deepest root first, so a folder emptied by its sub folder's removal goes in the same run.
    for (size_t i = 1; i < nroots; ++i) {
        for (size_t j = i; j > 0 && wcslen(roots[j]) > wcslen(roots[j - 1]); --j) {
            wchar_t *t = roots[j];
            roots[j] = roots[j - 1];
            roots[j - 1] = t;
        }
    }
    static wchar_t seenfile[PATH_CAP];
    swprintf(seenfile, PATH_CAP, L"%ls\\seen.txt", folder);
    load_seen(seenfile);
    int left = pending(machine);
    for (size_t i = 0; i < nfiles; ++i) {
        bool inside = false;
        for (size_t r = 0; r < nroots; ++r) {
            if (!under(files[i], roots[r])) continue;
            inside = seen[r] = true;
        }
        if (inside && !gone(files[i])) ++left;
    }
    // A package folder above a seen one: the installer could not remove it either.
    for (size_t i = 0; i < nroots; ++i) {
        for (size_t j = 0; j < nroots; ++j) {
            if (seen[j] && i != j && under(roots[j], roots[i])) seen[i] = true;
        }
    }
    save_seen(seenfile);
    // A seen root goes once the product is no longer installed and the root is empty.
    bool installed = product[0] && MsiQueryProductStateW(product) == INSTALLSTATE_DEFAULT;
    for (size_t r = 0; r < nroots; ++r) {
        DWORD a = GetFileAttributesW(roots[r]);
        if (a == INVALID_FILE_ATTRIBUTES || installed) continue;
        if ((a & FILE_ATTRIBUTE_REPARSE_POINT) || !seen[r]) continue;     // only folders the installer had to leave
        if (empty_dir(roots[r])) RemoveDirectoryW(roots[r]);
    }
    SYSTEMTIME now;
    GetLocalTime(&now);
    wchar_t today[16];
    swprintf(today, 16, L"%04u%02u%02u", now.wYear, now.wMonth, now.wDay);
    if (left == 0 || (until[0] && wcscmp(today, until) > 0)) finish(folder);
    return 0;
}
