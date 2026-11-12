// src/clean/rubrapack_clean.c - the cleanup task's program (RFC-0026).
//
//   rubrapack_clean.exe <folder>
//
// The helper DLL's commit action puts this program and <folder>\list.txt in a folder of its own
// (%ProgramData%\rubrapack\cleanup\<ProductCode> for a per-machine package, the user's
// %LOCALAPPDATA%\rubrapack\cleanup\<ProductCode> for a per-user one) and registers a scheduled
// task that runs it at logon and every 15 minutes. Each run deletes what it can of:
//   - the deletions this installation queued for the next restart (PendingFileRenameOperations,
//     those not already queued before it ran: earlier ones are other installations', even in a shared
//     folder) under one of the list's roots (the package's folders), and, for a per-machine
//     package, the installer's backup copies of them (<volume>\Config.Msi\*.rbf) - each only
//     while the same file is at that path (a file installed there since is left alone);
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
// (the last day, YYYYMMDD, local time), product ({ProductCode}), scope (machine or user), kind
// (remove, upgrade or maintenance), before (a relevant deletion already queued before the script), root
// (a folder), file (a file), parent (the main product's UpgradeCode) and addon ({ProductCode}).
// A removal's task deletes nothing once its product is installed again.
//
// Add-ons (x46): a removal of a product with [package] remove-addons lists the add-ons installed
// under SOFTWARE\rubrapack\Addons\<parent>. The task is started at once; this program waits for
// the removal to end, then runs msiexec /x for each add-on that is still installed and still named
// there. One that fails (another installation running) is tried again on the next run.

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>
#include <msi.h>
#include <msiquery.h>
#include <shellapi.h>

#include "../ca/rp_wstr.h"
#include "fileop.h"
#include "keypath.h"
#include "pfro.h"

enum { MAX_ITEMS = 256, PATH_CAP = 32768 };

static wchar_t task[512], product[64], scope[16], until[16], kind[16], parent[64];
static wchar_t addons[64][40];
static size_t naddons;
static wchar_t *roots[MAX_ITEMS], *files[MAX_ITEMS];
static bool seen[MAX_ITEMS];        // a root that held a queued deletion (seen.txt keeps it between runs)
static size_t nroots, nfiles;
static wchar_t *befores[1024];      // deletions queued before the installation's script (list.txt "before")
static bool used_before[1024];
static size_t nbefores;

// Formats into out. A text that does not fit is not cut short (a path cut short is another path):
// out is then empty and the result -1, so whatever is done with it fails instead.
static int wfmt(wchar_t *out, size_t cap, const wchar_t *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vswprintf(out, cap, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap) {
        out[0] = 0;
        return -1;
    }
    return n;
}

// "{8-4-4-4-12}" in hex digits: a code goes into a registry path and msiexec's command line.
static bool is_guid(const wchar_t *s) {
    if (rp_wlen(s) != 38 || s[0] != L'{' || s[37] != L'}') return false;
    for (int i = 1; i < 37; ++i) {
        bool dash = i == 9 || i == 14 || i == 19 || i == 24;
        wchar_t c = s[i];
        bool hex = (c >= L'0' && c <= L'9') || (c >= L'A' && c <= L'F') || (c >= L'a' && c <= L'f');
        if (dash ? c != L'-' : !hex) return false;
    }
    return true;
}

// With RUBRAPACK_CLEAN_LOG set (by hand, never by the task), each step is appended to that file.
static void say(const wchar_t *fmt, ...) {
    static wchar_t path[MAX_PATH];
    static int state;               // 0 unknown, 1 on, 2 off
    if (state == 0) state = GetEnvironmentVariableW(L"RUBRAPACK_CLEAN_LOG", path, MAX_PATH) - 1 < MAX_PATH - 1 ? 1 : 2;
    if (state != 1) return;
    wchar_t line[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vswprintf(line, 2046, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    line[n++] = L'\r';
    line[n++] = L'\n';
    HANDLE f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD put = 0;
    WriteFile(f, line, (DWORD)n * sizeof(wchar_t), &put, NULL);
    CloseHandle(f);
}

static wchar_t *text_dup(const wchar_t *s) {
    size_t n = rp_wlen(s) + 1;
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
    if (!rp_wstarts(p, L"RPC1")) return false;
    rp_wtok_t lines = rp_wtok(p, L"\r\n");
    for (wchar_t *line = rp_wtok_next(&lines); line; line = rp_wtok_next(&lines)) {
        wchar_t *tab = rp_wchr(line, L'\t');
        if (tab == NULL) continue;
        *tab = 0;
        const wchar_t *v = tab + 1;
        if (rp_weq(line, L"task")) rp_wcpy_cut(task, 512, v);
        else if (rp_weq(line, L"product")) rp_wcpy_cut(product, 64, v);
        else if (rp_weq(line, L"scope")) rp_wcpy_cut(scope, 16, v);
        else if (rp_weq(line, L"until")) rp_wcpy_cut(until, 16, v);
        else if (rp_weq(line, L"before") && nbefores < 1024) befores[nbefores++] = text_dup(v);
        else if (rp_weq(line, L"kind")) rp_wcpy_cut(kind, 16, v);
        else if (rp_weq(line, L"root") && nroots < MAX_ITEMS) roots[nroots++] = text_dup(v);
        else if (rp_weq(line, L"file") && nfiles < MAX_ITEMS) files[nfiles++] = text_dup(v);
        else if (rp_weq(line, L"parent")) rp_wcpy_cut(parent, 64, v);
        else if (rp_weq(line, L"addon") && naddons < 64 && is_guid(v)) (void)RP_WCPY(addons[naddons++], v);
    }
    if (product[0] && !is_guid(product)) return false;
    if (parent[0] && !is_guid(parent)) return false;
    return task[0] != 0;
}

// Whether `path` is inside folder `root` (not the root itself), ignoring case (keypath.h).
static bool under(const wchar_t *path, const wchar_t *root) { return keypath_inside(path, root); }

// <volume>\Config.Msi\<name>.rbf, one level down (keypath.h).
static bool installer_backup(const wchar_t *path) { return keypath_is_backup(path); }

// `path` in fileop.h's form ("\\?\C:\..." without a '\' at the end, as a handle names it), into
// `out`; false when it is not a path on a drive.
static bool long_form(const wchar_t *path, wchar_t *out, size_t cap) {
    if (!keypath_is_drive_path(path) || !rp_wcpy(out, cap, L"\\\\?\\") || !rp_wcat(out, cap, path)) return false;
    size_t n = rp_wlen(out);
    while (n > 7 && out[n - 1] == L'\\') out[--n] = 0;      // "\\?\C:\" keeps its own
    return true;
}

// Where folder `dir` is, as Windows finds it (links on the way followed), into `out`.
static bool real_place(const wchar_t *dir, wchar_t *out, size_t cap) {
    static wchar_t lp[PATH_CAP];
    if (!long_form(dir, lp, PATH_CAP)) return false;
    HANDLE d = CreateFileW(lp, FILE_READ_ATTRIBUTES | SYNCHRONIZE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (d == INVALID_HANDLE_VALUE) return false;
    DWORD n = GetFinalPathNameByHandleW(d, out, (DWORD)cap, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(d);
    return n != 0 && n < cap;
}

// Where `path` is when nothing below its root is a link: the root's own place (it may be reached
// through a link above it - a profile moved to another drive) and the rest of `path` as written.
// For a root itself, or a path under no root (an installer backup), its folder's place and its
// name: a link above it is followed, the entry itself is opened as itself. False when it cannot
// be told.
static bool expected_place(const wchar_t *path, wchar_t *out, size_t cap) {
    const wchar_t *root = NULL;
    for (size_t r = 0; r < nroots; ++r) {
        if (under(path, roots[r]) && (root == NULL || rp_wlen(roots[r]) > rp_wlen(root))) root = roots[r];
    }
    static wchar_t base[PATH_CAP];
    size_t bl;
    if (root) {
        bl = rp_wlen(root);
        while (bl && root[bl - 1] == L'\\') --bl;
    } else {
        bl = rp_wlen(path);
        while (bl && path[bl - 1] == L'\\') --bl;      // the entry's own name, without a '\\' after it
        size_t end = bl;
        while (bl && path[bl - 1] != L'\\') --bl;
        if (bl < 3 || end == bl) return long_form(path, out, cap);       // a volume's root
        --bl;                                                              // its folder, without the '\\'
        root = path;
    }
    if (bl == 0 || bl >= PATH_CAP) return false;
    memcpy(base, root, bl * sizeof *base);
    base[bl] = 0;
    if (bl == 2) (void)rp_wcat(base, PATH_CAP, L"\\");             // "C:" is "C:\\"
    if (!real_place(base, out, cap)) return false;
    size_t on = rp_wlen(out);
    if (on && out[on - 1] == L'\\') out[--on] = 0;
    // The rest, without a '\\' at its end (a handle names a folder without one).
    static wchar_t rest[PATH_CAP];
    if (!rp_wcpy(rest, PATH_CAP, path + bl)) return false;
    size_t rn = rp_wlen(rest);
    while (rn > 1 && rest[rn - 1] == L'\\') rest[--rn] = 0;
    return rp_wcat(out, cap, rest);
}

// Opens `path` as itself with `access` (fileop.h), where it is expected to be (expected_place:
// nothing below its root is a link); INVALID_HANDLE_VALUE otherwise - and *missing when there is
// nothing there at all.
static HANDLE open_here(const wchar_t *path, DWORD access, DWORD *attr, bool *missing) {
    static wchar_t lp[PATH_CAP], want[PATH_CAP], real[PATH_CAP];
    *missing = false;
    if (!long_form(path, lp, PATH_CAP)) return INVALID_HANDLE_VALUE;
    HANDLE f = fileop_open_as(lp, access, real, PATH_CAP, attr);
    if (f == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        *missing = e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND;
        return f;
    }
    if (!expected_place(path, want, PATH_CAP) || CompareStringOrdinal(real, -1, want, -1, TRUE) != CSTR_EQUAL) {
        say(L"%ls: it is at %ls, not where its name says", path, real);
        CloseHandle(f);
        return INVALID_HANDLE_VALUE;
    }
    return f;
}

// The identity of the file `f` is open on ("" when it cannot be read).
static void handle_id(HANDLE f, wchar_t id[40]) {
    id[0] = 0;
    BY_HANDLE_FILE_INFORMATION fi;
    if (GetFileInformationByHandle(f, &fi))
        wfmt(id, 40, L"%08lX%08lX%08lX", (unsigned long)fi.dwVolumeSerialNumber, (unsigned long)fi.nFileIndexHigh, (unsigned long)fi.nFileIndexLow);
}

// What this installation left: found once, on the first run - the queued deletions that were not
// already queued before its script ran (list.txt `before`; those belong to other installations,
// even in the same folder; paths, not positions, since other tasks take entries out - x42)
// under a root or, per machine, in Config.Msi, and the list's `file` lines - each with the identity
// of the file there then (volume serial and file index). Later runs delete a path only while the
// same file is there: a file installed at that path since (a reinstallation) is left alone.
typedef struct {
    wchar_t *path;
    wchar_t  id[40];
    bool     active;
    bool     noted;                 // a `file` line (could not be queued), not a queued deletion
} own_t;
static own_t own[MAX_ITEMS];
static size_t nown;
static bool scanned;

// The identity of the file at `path` ("" when there is none, when it is not where its name says -
// a folder above it put elsewhere by a link - or when it is a link itself: never this installation's).
static void file_id(const wchar_t *path, wchar_t id[40]) {
    id[0] = 0;
    DWORD attr = 0;
    bool missing;
    HANDLE f = open_here(path, FILE_READ_ATTRIBUTES, &attr, &missing);     // a held file opens too
    if (f == INVALID_HANDLE_VALUE) return;
    if (!(attr & FILE_ATTRIBUTE_REPARSE_POINT)) handle_id(f, id);
    CloseHandle(f);
}

// Deletes the file or empty folder at `path` while it is the one of identity `id` (NULL: any folder
// that is not a link), through one handle that is where `path` says: nothing between the check and
// the deletion can put another file there. True when it is gone (or was not there).
static bool delete_own(const wchar_t *path, const wchar_t *id) {
    DWORD attr = 0;
    bool missing;
    HANDLE f = open_here(path, DELETE | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES, &attr, &missing);
    if (f == INVALID_HANDLE_VALUE) f = open_here(path, DELETE | FILE_READ_ATTRIBUTES, &attr, &missing);
    if (f == INVALID_HANDLE_VALUE) return missing;
    bool ok = !(attr & FILE_ATTRIBUTE_REPARSE_POINT);
    if (ok && id) {
        wchar_t now[40];
        handle_id(f, now);
        ok = now[0] && rp_weq(now, id);
    } else if (ok) {
        ok = (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }
    ok = ok && fileop_delete(f, attr);
    CloseHandle(f);
    return ok;
}

// Takes `path` as this installation's when it is under a root (marking the root) or, per machine,
// one of the installer's backups.
static void take(const wchar_t *path, bool machine, bool noted) {
    bool mine = machine && installer_backup(path);
    for (size_t i = 0; i < nroots; ++i) {
        if (!under(path, roots[i])) continue;
        seen[i] = mine = true;
    }
    if (!mine || nown >= MAX_ITEMS) return;
    own_t *o = &own[nown];
    file_id(path, o->id);
    say(L"take %ls: id '%ls' (error %lu)", path, o->id, (unsigned long)GetLastError());
    if (o->id[0] == 0) return;              // already gone
    o->path = text_dup(path);
    o->noted = noted;
    o->active = o->path != NULL;
    nown += o->active;
}

// The first run's scan of the queued deletions.
static void scan(bool machine) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager", 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS)
        return;
    DWORD type = 0, size = 0;
    if (RegQueryValueExW(k, L"PendingFileRenameOperations", NULL, &type, NULL, &size) == ERROR_SUCCESS && type == REG_MULTI_SZ && size) {
        wchar_t *buf = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size + 2 * sizeof(wchar_t));
        if (buf && RegQueryValueExW(k, L"PendingFileRenameOperations", NULL, &type, (BYTE *)buf, &size) == ERROR_SUCCESS) {
            // Pairs of source and target; an empty target is a deletion.
            unsigned long index = 0;
            for (wchar_t *src = buf; *src; ++index) {
                wchar_t *dst = src + rp_wlen(src) + 1;
                if (*dst == 0) {
                    // One that was queued before the installation's script is not this installation's
                    // (matched path for path: the same path may be queued more than once).
                    const wchar_t *path = pfro_path(src);
                    bool earlier = false;
                    for (size_t i = 0; i < nbefores && !earlier; ++i) {
                        if (used_before[i] || CompareStringOrdinal(befores[i], -1, path, -1, TRUE) != CSTR_EQUAL) continue;
                        used_before[i] = earlier = true;
                    }
                    say(L"queued #%lu %ls%ls", index, path, earlier ? L" (before)" : L"");
                    if (!earlier) take(path, machine, false);
                }
                src = *dst ? dst + rp_wlen(dst) + 1 : dst + 1;
            }
        }
        if (buf) HeapFree(GetProcessHeap(), 0, buf);
    }
    RegCloseKey(k);
}

// Whether an installed product has a file at `path`: a component whose key path is a file in the
// same folder, listing a file of that name (the File table of the product's cached package). Another
// installation that put an identical file there kept the one already present (x41), so the file's
// identity cannot tell; this can. Built once per run, only when something is about to be deleted.
static wchar_t **owned;
static size_t nowned, capowned;
static bool owned_built;

static void own_path(const wchar_t *dir, const wchar_t *name) {
    if (nowned == capowned) {
        size_t cap = capowned ? capowned * 2 : 256;
        wchar_t **n = owned ? HeapReAlloc(GetProcessHeap(), 0, owned, cap * sizeof *n) : HeapAlloc(GetProcessHeap(), 0, cap * sizeof *n);
        if (n == NULL) return;
        owned = n;
        capowned = cap;
    }
    size_t a = rp_wlen(dir), b = rp_wlen(name);
    wchar_t *p = HeapAlloc(GetProcessHeap(), 0, (a + b + 1) * sizeof *p);
    if (p == NULL) return;
    memcpy(p, dir, a * sizeof *p);
    memcpy(p + a, name, (b + 1) * sizeof *p);
    owned[nowned++] = p;
}

static void scan_product(const wchar_t *code) {
    static wchar_t pkg[PATH_CAP], keypath[PATH_CAP], dir[PATH_CAP], comp[80], cid[80], fname[1024];
    DWORD n = PATH_CAP;
    if (MsiGetProductInfoW(code, INSTALLPROPERTY_LOCALPACKAGE, pkg, &n) != ERROR_SUCCESS) return;
    MSIHANDLE db = 0, view = 0, rec = 0;
    if (MsiOpenDatabaseW(pkg, MSIDBOPEN_READONLY, &db) != ERROR_SUCCESS) return;
    if (MsiDatabaseOpenViewW(db, L"SELECT `Component`.`ComponentId`, `File`.`FileName` FROM `File`, `Component` "
                                 L"WHERE `File`.`Component_` = `Component`.`Component` ORDER BY `Component`.`ComponentId`", &view) == ERROR_SUCCESS &&
        MsiViewExecute(view, 0) == ERROR_SUCCESS) {
        comp[0] = 0;
        bool have_dir = false;
        while (MsiViewFetch(view, &rec) == ERROR_SUCCESS) {
            DWORD cn = 80, fn = 1024;
            if (MsiRecordGetStringW(rec, 1, cid, &cn) == ERROR_SUCCESS && MsiRecordGetStringW(rec, 2, fname, &fn) == ERROR_SUCCESS) {
                if (!rp_weq(cid, comp)) {            // a new component: where its key path is
                    (void)RP_WCPY(comp, cid);
                    DWORD kn = PATH_CAP;
                    // keypath.h: a file's or folder's key path, or a registry one holding the folder
                    have_dir = MsiGetComponentPathW(code, cid, keypath, &kn) == INSTALLSTATE_LOCAL && keypath_dir(keypath, dir, sizeof dir / sizeof *dir);
                }
                if (have_dir) {
                    wchar_t *bar = rp_wchr(fname, L'|');      // "SHORT|Long name"
                    own_path(dir, bar ? bar + 1 : fname);
                }
            }
            MsiCloseHandle(rec);
        }
    }
    if (view) MsiCloseHandle(view);
    MsiCloseHandle(db);
}

static bool owned_by_installed_product(const wchar_t *path) {
    if (!owned_built) {
        owned_built = true;
        wchar_t code[39];
        for (DWORD i = 0; MsiEnumProductsW(i, code) == ERROR_SUCCESS; ++i) scan_product(code);
    }
    for (size_t i = 0; i < nowned; ++i) {
        if (CompareStringOrdinal(owned[i], -1, path, -1, TRUE) == CSTR_EQUAL) return true;
    }
    return false;
}

// Deletes what is still this installation's; returns how many are still there afterwards.
static int pending(bool machine) {
    if (!scanned) {
        scan(machine);
        for (size_t i = 0; i < nfiles; ++i) take(files[i], false, true);
        scanned = true;
    }
    // A removal whose product is installed again: what it left now belongs to that installation.
    bool back = rp_weq(kind, L"remove") && product[0] && MsiQueryProductStateW(product) == INSTALLSTATE_DEFAULT;
    int left = 0;
    for (size_t i = 0; i < nown; ++i) {
        own_t *o = &own[i];
        if (back) o->active = false;
        if (!o->active) continue;
        wchar_t id[40];
        file_id(o->path, id);
        if (id[0] == 0 || !rp_weq(id, o->id)) {         // gone, or another file now
            say(L"%ls: gone or another file ('%ls')", o->path, id);
            o->active = false;
            continue;
        }
        // Its deletion taken back (a later installation that kept the file did it, pfro.h), or the
        // file now belongs to an installed product: then it is not this installation's any more -
        // and per machine its old deletion is taken back here, so the restart keeps it too.
        if (!o->noted && !pfro_has(o->path)) {
            say(L"%ls: its deletion was taken back", o->path);
            o->active = false;
            continue;
        }
        if (!installer_backup(o->path) && owned_by_installed_product(o->path)) {
            if (machine) pfro_cancel(o->path, (unsigned long)-1);
            say(L"%ls: an installed product has it", o->path);
            o->active = false;
            continue;
        }
        bool deleted = delete_own(o->path, o->id);
        say(L"%ls: %ls (error %lu)", o->path, deleted ? L"deleted" : L"still there", (unsigned long)GetLastError());
        if (!deleted) ++left;
        else o->active = false;
    }
    return left;
}

// x46: whether the add-on is still named under the main product's Addons key (either view).
static bool addon_listed(const wchar_t *code, bool machine) {
    wchar_t key[160];
    wfmt(key, 160, L"SOFTWARE\\rubrapack\\Addons\\%ls", parent);
    static const REGSAM views[] = { KEY_WOW64_64KEY, KEY_WOW64_32KEY };
    for (int v = 0; v < 2; ++v) {
        HKEY k;
        if (RegOpenKeyExW(machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER, key, 0, KEY_QUERY_VALUE | views[v], &k) != ERROR_SUCCESS) continue;
        bool hit = RegQueryValueExW(k, code, NULL, NULL, NULL, NULL) == ERROR_SUCCESS;
        RegCloseKey(k);
        if (hit) return true;
    }
    return false;
}

// The Addons\<parent> key once no add-on is named in it (the add-ons' removal deletes their values).
static void addons_key_cleanup(bool machine) {
    wchar_t key[160];
    wfmt(key, 160, L"SOFTWARE\\rubrapack\\Addons\\%ls", parent);
    static const REGSAM views[] = { KEY_WOW64_64KEY, KEY_WOW64_32KEY };
    HKEY root = machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
    for (int v = 0; v < 2; ++v) {
        HKEY k;
        if (RegOpenKeyExW(root, key, 0, KEY_QUERY_VALUE | views[v], &k) != ERROR_SUCCESS) continue;
        DWORD subkeys = 1, values = 1;
        RegQueryInfoKeyW(k, NULL, NULL, NULL, &subkeys, NULL, NULL, &values, NULL, NULL, NULL, NULL);
        RegCloseKey(k);
        if (subkeys == 0 && values == 0) RegDeleteKeyExW(root, key, views[v], 0);
    }
}

// Removes the add-ons still installed; returns how many are left to try again.
static int remove_addons(bool machine) {
    if (naddons == 0 || parent[0] == 0) return 0;
    // The main product installed again: its add-ons stay.
    if (product[0] && MsiQueryProductStateW(product) == INSTALLSTATE_DEFAULT) {
        say(L"add-ons: %ls is installed again; they stay", product);
        return 0;
    }
    static wchar_t cmd[PATH_CAP], sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    int left = 0;
    for (size_t i = 0; i < naddons; ++i) {
        if (MsiQueryProductStateW(addons[i]) != INSTALLSTATE_DEFAULT) {
            say(L"add-on %ls: not installed", addons[i]);
            continue;
        }
        if (!addon_listed(addons[i], machine)) {      // not this product's add-on any more
            say(L"add-on %ls: no longer named under %ls", addons[i], parent);
            continue;
        }
        wfmt(cmd, PATH_CAP, L"\"%ls\\msiexec.exe\" /x %ls /qn /norestart", sys, addons[i]);
        STARTUPINFOW si = { .cb = sizeof si };
        PROCESS_INFORMATION pi;
        DWORD code = (DWORD)-1;
        if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, 240000);
            GetExitCodeProcess(pi.hProcess, &code);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }
        say(L"add-on %ls: msiexec /x -> %lu", addons[i], (unsigned long)code);
        // 0, 3010 (a held file goes at the restart), 1605 (not installed after all) are done.
        if (code != 0 && code != 3010 && code != 1605) ++left;
    }
    if (left == 0) addons_key_cleanup(machine);
    return left;
}

// seen.txt (UTF-16LE): "scanned", then "root<TAB>path" for each root that held one of this
// installation's deletions and "own<TAB>identity<TAB>path" for each item still to delete.
static void load_seen(const wchar_t *path) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD size = GetFileSize(h, NULL), got = 0;
    wchar_t *t = size != INVALID_FILE_SIZE && size < (1u << 20) && size % 2 == 0 ? HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size + 2) : NULL;
    if (t && ReadFile(h, t, size, &got, NULL) && got == size) {
        rp_wtok_t lines = rp_wtok(t, L"\r\n");
        for (wchar_t *line = rp_wtok_next(&lines); line; line = rp_wtok_next(&lines)) {
            if (rp_weq(line, L"scanned")) scanned = true;
            bool noted = rp_wstarts(line, L"note\t");
            if ((noted || rp_wstarts(line, L"own\t")) && nown < MAX_ITEMS) {
                const wchar_t *v = line + (noted ? 5 : 4);
                wchar_t *tab = rp_wchr(v, L'\t');
                if (tab == NULL || tab - v >= 40) continue;
                own_t *o = &own[nown];
                memcpy(o->id, v, (size_t)(tab - v) * sizeof(wchar_t));
                o->id[tab - v] = 0;
                o->noted = noted;
                o->path = text_dup(tab + 1);
                o->active = o->path != NULL;
                nown += o->active;
            }
            if (!rp_wstarts(line, L"root\t")) continue;
            for (size_t i = 0; i < nroots; ++i) {
                if (CompareStringOrdinal(line + 5, -1, roots[i], -1, TRUE) == CSTR_EQUAL) seen[i] = true;
            }
        }
    }
    CloseHandle(h);
}

static void put_line(HANDLE h, const wchar_t *a, const wchar_t *b, const wchar_t *c) {
    DWORD put = 0;
    WriteFile(h, a, (DWORD)(rp_wlen(a) * sizeof(wchar_t)), &put, NULL);
    if (b) WriteFile(h, b, (DWORD)(rp_wlen(b) * sizeof(wchar_t)), &put, NULL);
    if (c) {
        WriteFile(h, L"\t", sizeof(wchar_t), &put, NULL);
        WriteFile(h, c, (DWORD)(rp_wlen(c) * sizeof(wchar_t)), &put, NULL);
    }
    WriteFile(h, L"\r\n", 2 * sizeof(wchar_t), &put, NULL);
}

static void save_seen(const wchar_t *path) {
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    if (scanned) put_line(h, L"scanned", NULL, NULL);
    for (size_t i = 0; i < nroots; ++i) {
        if (seen[i]) put_line(h, L"root\t", roots[i], NULL);
    }
    for (size_t i = 0; i < nown; ++i) {
        if (own[i].active) put_line(h, own[i].noted ? L"note\t" : L"own\t", own[i].id, own[i].path);
    }
    CloseHandle(h);
}

static bool empty_dir(const wchar_t *dir) {
    static wchar_t pat[PATH_CAP];
    wfmt(pat, PATH_CAP, L"%ls\\*", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(pat, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool empty = true;
    do {
        if (!rp_weq(fd.cFileName, L".") && !rp_weq(fd.cFileName, L"..")) empty = false;
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
    wfmt(cmd, PATH_CAP, L"\"%ls\\schtasks.exe\" /delete /tn \"%ls\" /f", sys, task);
    if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 30000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    // The parent folders go too when this was the last one (rd leaves a folder that is not empty).
    wfmt(cmd, PATH_CAP, L"\"%ls\\cmd.exe\" /c ping -n 3 127.0.0.1 >nul & rd /s /q \"%ls\" & rd \"%ls\\..\" 2>nul & rd \"%ls\\..\\..\" 2>nul",
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
    size_t fl = rp_wlen(folder);
    while (fl > 3 && folder[fl - 1] == L'\\') folder[--fl] = 0;
    wfmt(list, PATH_CAP, L"%ls\\list.txt", folder);
    if (!read_list(list)) return 3;
    // Not while Windows Installer runs an installation (its backups may still be needed). With
    // add-ons to remove, the removal that started this run is still ending: wait for it (x46).
    for (int waited = 0;; waited += 2) {
        HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, L"Global\\_MSIExecute");
        if (m == NULL) break;
        CloseHandle(m);
        if (naddons == 0 || waited >= 300) return 0;
        Sleep(2000);
    }
    bool machine = rp_weq(scope, L"machine");
    // The deepest root first, so a folder emptied by its sub folder's removal goes in the same run.
    for (size_t i = 1; i < nroots; ++i) {
        for (size_t j = i; j > 0 && rp_wlen(roots[j]) > rp_wlen(roots[j - 1]); --j) {
            wchar_t *t = roots[j];
            roots[j] = roots[j - 1];
            roots[j - 1] = t;
        }
    }
    static wchar_t seenfile[PATH_CAP];
    wfmt(seenfile, PATH_CAP, L"%ls\\seen.txt", folder);
    load_seen(seenfile);
    say(L"run: task '%ls' kind '%ls', %zu earlier deletion(s), %zu root(s)", task, kind, nbefores, nroots);
    int left = remove_addons(machine);
    left += pending(machine);
    say(L"left %d", left);
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
        if (empty_dir(roots[r])) (void)delete_own(roots[r], NULL);
    }
    SYSTEMTIME now;
    GetLocalTime(&now);
    wchar_t today[16];
    wfmt(today, 16, L"%04u%02u%02u", now.wYear, now.wMonth, now.wDay);
    if (left == 0 || (until[0] && rp_wcmp(today, until) > 0)) finish(folder);
    return 0;
}
