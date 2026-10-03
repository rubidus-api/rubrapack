// src/ca/rubrapack_ca.c - rubrapack's helper custom-action DLL (RFC-0001 9.6, 9.6.1; RFC-0004).
//
// Built for x64, x86 and Arm64 by `nob parts` and stored in resources/bin/; `build` puts the one
// for the package's architecture into the Binary table. It depends on the OS only (msi.dll,
// advapi32, kernel32, user32, comdlg32 and the Universal CRT).
//
// REG_QWORD values (the Registry table cannot write them):
//   RpQwordPrepare  immediate: reads the plan from the RP_QWORDS property, decides per value from
//                   its component's action state (install: write, removal: delete), reads what is
//                   there now, and hands both lists to the deferred actions as CustomActionData.
//   RpQwordApply    deferred: writes or deletes the values.
//   RpQwordRollback deferred rollback: puts back what RpQwordPrepare saw.
//
// Install folder guard (RFC-0012 V6):
//   RpGuardDirs     immediate, execute sequence, first installation only: every directory named in
//                   RP_GUARD (';'-separated Directory keys) must not pass through a reparse point,
//                   and when it already exists it must be owned by SYSTEM, Administrators or
//                   TrustedInstaller - otherwise the error message RpGuardMsg_<RPLANGUAGE> (or
//                   RpGuardMsg_en; [1] = the folder) and the installation stops before any file is
//                   placed.
//
// Saving the log (RFC-0020):
//   RpSaveLog       DoAction of the Save log button on the last pages: asks where to save (the
//                   standard Save As window) and copies the log of this run (MsiLogFileLocation)
//                   there; on failure the message RpLogMsg_<RPLANGUAGE> (or RpLogMsg_en; [1] = the
//                   file). It never fails the installation.
//
// [remove] upgrade = false (DECISIONS 2026-10-02):
//   RpRemovePrepare immediate: nothing when UPGRADINGPRODUCTCODE is set (an upgrade removing this
//                   version); otherwise, for each plan record (Directory key, pattern or "" for the
//                   folder itself, component) whose component is being removed, lists the matching
//                   files and gives each a place in a backup folder (RP_REMOVES_ROOT "volume": the
//                   file's volume's Config.Msi, as Windows Installer's own backups; "temp": the
//                   user's temporary folder).
//   RpRemoveApply   deferred: moves the files there (a file that cannot be moved, being held, is
//                   left and deleted at the next restart).
//   RpRemoveRollback deferred rollback: moves them back.
//   RpRemoveCommit  commit: deletes the backups (at the next restart if one is held) and the
//                   folders the plan names, when empty.
//   None of them fails the installation: a file left behind is not worth a rollback.
//
// Cleaning up later (RFC-0026):
//   RpCleanupPrepare immediate: the package's folders (RP_CLEANUP_DIRS, Directory keys), its scope
//                   and ProductCode, and where its cleanup folder is, for the commit action.
//   RpCleanupRegister commit: for a run that removes or replaces files (removal, upgrade,
//                   maintenance) or that noted files a per-user package could not queue
//                   (pending.txt), puts rubrapack_clean.exe (embedded in this DLL) and its list in
//                   the cleanup folder and registers the scheduled task "rubrapack cleanup
//                   <ProductCode>" (first run two minutes later, then at logon and every 15
//                   minutes, for 30 days). Windows Installer queues its own deletions for the next
//                   restart only after the commit actions, so the task looks then; it removes itself
//                   when nothing is left. It never fails the installation.
//
// Data format (RFC-0001 9.6.1): "RPQ1" followed by records; every field is "<decimal length>:" and
// that many UTF-16 units, so any text (including ':' and ';') round-trips. A plan record is
// root, view, key, name, value (16 hex digits), component, keep; an apply record is op ("w" write,
// "d" delete), root, view, key, name, value.

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>
#include <msi.h>
#include <msiquery.h>
#include <aclapi.h>
#include <sddl.h>
#include <commdlg.h>

#include "../clean/pfro.h"

// rubrapack_clean.exe for this architecture, as a byte array (written by `nob parts`).
#if __has_include("rp_clean_part.h")
#include "rp_clean_part.h"
#else
static const unsigned char rp_clean_part[] = { 0 };
static const size_t rp_clean_part_len = 0;
#endif

enum { MAX_TEXT = 1 << 16, MAX_FIELDS = 16 };

typedef struct {
    const wchar_t *p;
    const wchar_t *end;
} reader_t;

typedef struct {
    wchar_t *buf;
    size_t   len, cap;
    bool     bad;
} writer_t;

static void w_raw(writer_t *w, const wchar_t *s, size_t n) {
    if (w->bad) return;
    if (w->len + n + 1 > w->cap) {
        size_t cap = w->cap ? w->cap * 2 : 1024;
        while (cap < w->len + n + 1) cap *= 2;
        wchar_t *nb = w->buf ? HeapReAlloc(GetProcessHeap(), 0, w->buf, cap * sizeof *nb)
                             : HeapAlloc(GetProcessHeap(), 0, cap * sizeof *nb);
        if (nb == NULL) {
            w->bad = true;
            return;
        }
        w->buf = nb;
        w->cap = cap;
    }
    memcpy(w->buf + w->len, s, n * sizeof *s);
    w->len += n;
    w->buf[w->len] = 0;
}

static void w_field(writer_t *w, const wchar_t *s) {
    wchar_t head[24];
    size_t n = wcslen(s);
    int h = swprintf(head, 24, L"%zu:", n);
    w_raw(w, head, (size_t)h);
    w_raw(w, s, n);
}

// Reads one field into out (NUL-terminated); false at the end or on a malformed length.
static bool r_field(reader_t *r, wchar_t *out, size_t cap) {
    size_t n = 0;
    const wchar_t *p = r->p;
    if (p >= r->end || *p < L'0' || *p > L'9') return false;
    while (p < r->end && *p >= L'0' && *p <= L'9') {
        n = n * 10 + (size_t)(*p - L'0');
        if (n >= cap) return false;
        ++p;
    }
    if (p >= r->end || *p != L':' || (size_t)(r->end - p - 1) < n) return false;
    memcpy(out, p + 1, n * sizeof *out);
    out[n] = 0;
    r->p = p + 1 + n;
    return true;
}

static wchar_t *get_property(MSIHANDLE h, const wchar_t *name) {
    DWORD n = 0;
    wchar_t probe[1] = L"";
    if (MsiGetPropertyW(h, name, probe, &n) != ERROR_MORE_DATA && n == 0) {
        wchar_t *empty = HeapAlloc(GetProcessHeap(), 0, sizeof(wchar_t));
        if (empty) *empty = 0;
        return empty;
    }
    ++n;
    wchar_t *s = HeapAlloc(GetProcessHeap(), 0, n * sizeof *s);
    if (s && MsiGetPropertyW(h, name, s, &n) != ERROR_SUCCESS) {
        HeapFree(GetProcessHeap(), 0, s);
        s = NULL;
    }
    return s;
}

static void log_line(MSIHANDLE h, const wchar_t *fmt, ...) {
    wchar_t text[1024];
    va_list ap;
    va_start(ap, fmt);
    vswprintf(text, 1024, fmt, ap);
    va_end(ap);
    MSIHANDLE rec = MsiCreateRecord(0);
    MsiRecordSetStringW(rec, 0, text);
    MsiProcessMessage(h, INSTALLMESSAGE_INFO, rec);
    MsiCloseHandle(rec);
}

// root: "HKLM", "HKCU", "HKCR", or "HKMU" (resolved by the caller); view: "32" or "64".
static HKEY root_key(const wchar_t *root) {
    if (wcscmp(root, L"HKCU") == 0) return HKEY_CURRENT_USER;
    if (wcscmp(root, L"HKCR") == 0) return HKEY_CLASSES_ROOT;
    return HKEY_LOCAL_MACHINE;
}

static REGSAM view_flag(const wchar_t *view) { return wcscmp(view, L"32") == 0 ? KEY_WOW64_32KEY : KEY_WOW64_64KEY; }

static UINT set_data(MSIHANDLE h, const wchar_t *action, writer_t *w) {
    UINT rc = MsiSetPropertyW(h, action, w->bad ? L"" : (w->buf ? w->buf : L"RPQ1"));
    return w->bad ? ERROR_OUTOFMEMORY : rc;
}

__declspec(dllexport) UINT __stdcall RpQwordPrepare(MSIHANDLE h) {
    wchar_t *plan = get_property(h, L"RP_QWORDS");
    wchar_t *allusers = get_property(h, L"ALLUSERS");
    if (plan == NULL || allusers == NULL) return ERROR_INSTALL_FAILURE;
    bool machine = allusers[0] == L'1';
    reader_t r = { plan, plan + wcslen(plan) };
    writer_t apply = { 0 }, rollback = { 0 };
    w_raw(&apply, L"RPQ1", 4);
    w_raw(&rollback, L"RPQ1", 4);
    UINT rc = ERROR_SUCCESS;
    if (wcsncmp(plan, L"RPQ1", 4) != 0) rc = ERROR_INSTALL_FAILURE;
    r.p += 4;
    static wchar_t f[7][MAX_TEXT / 8];
    while (rc == ERROR_SUCCESS && r.p < r.end) {
        for (int i = 0; i < 7; ++i) {
            if (!r_field(&r, f[i], MAX_TEXT / 8)) rc = ERROR_INSTALL_FAILURE;
        }
        if (rc != ERROR_SUCCESS) break;
        const wchar_t *root = wcscmp(f[0], L"HKMU") == 0 ? (machine ? L"HKLM" : L"HKCU") : f[0];
        INSTALLSTATE installed = INSTALLSTATE_UNKNOWN, action = INSTALLSTATE_UNKNOWN;
        MsiGetComponentStateW(h, f[5], &installed, &action);
        bool keep = f[6][0] == L'1';
        const wchar_t *op = action == INSTALLSTATE_LOCAL || action == INSTALLSTATE_SOURCE ? L"w"
                          : action == INSTALLSTATE_ABSENT && !keep                    ? L"d"
                                                                                       : NULL;
        if (op == NULL) continue;
        // What is there now: the rollback puts exactly this back.
        HKEY k;
        uint64_t old = 0;
        DWORD type = 0, n = sizeof old;
        bool had = false;
        if (RegOpenKeyExW(root_key(root), f[2], 0, KEY_QUERY_VALUE | view_flag(f[1]), &k) == ERROR_SUCCESS) {
            had = RegQueryValueExW(k, f[3][0] ? f[3] : NULL, NULL, &type, (BYTE *)&old, &n) == ERROR_SUCCESS && type == REG_QWORD;
            RegCloseKey(k);
        }
        wchar_t oldhex[24];
        swprintf(oldhex, 24, L"%016llX", (unsigned long long)old);
        const wchar_t *rec[6] = { op, root, f[1], f[2], f[3], f[4] };
        for (int i = 0; i < 6; ++i) w_field(&apply, rec[i]);
        const wchar_t *back[6] = { had ? L"w" : L"d", root, f[1], f[2], f[3], oldhex };
        for (int i = 0; i < 6; ++i) w_field(&rollback, back[i]);
        log_line(h, L"rubrapack: qword %ls\\%ls\\%ls: %ls (was %ls)", root, f[2], f[3], op[0] == L'w' ? L"write" : L"delete",
                 had ? L"set" : L"absent");
    }
    if (rc == ERROR_SUCCESS) rc = set_data(h, L"RP_QwordApply", &apply);
    if (rc == ERROR_SUCCESS) rc = set_data(h, L"RP_QwordApplyRollback", &rollback);
    HeapFree(GetProcessHeap(), 0, plan);
    HeapFree(GetProcessHeap(), 0, allusers);
    if (apply.buf) HeapFree(GetProcessHeap(), 0, apply.buf);
    if (rollback.buf) HeapFree(GetProcessHeap(), 0, rollback.buf);
    return rc;
}

// Runs an apply list (RpQwordApply and RpQwordRollback share it).
static UINT run_list(MSIHANDLE h, bool rollback) {
    wchar_t *data = get_property(h, L"CustomActionData");
    if (data == NULL) return rollback ? ERROR_SUCCESS : ERROR_INSTALL_FAILURE;
    reader_t r = { data, data + wcslen(data) };
    UINT rc = ERROR_SUCCESS;
    if (wcsncmp(data, L"RPQ1", 4) != 0) rc = ERROR_INSTALL_FAILURE;
    r.p += 4;
    static wchar_t f[6][MAX_TEXT / 8];
    while (rc == ERROR_SUCCESS && r.p < r.end) {
        for (int i = 0; i < 6; ++i) {
            if (!r_field(&r, f[i], MAX_TEXT / 8)) rc = ERROR_INSTALL_FAILURE;
        }
        if (rc != ERROR_SUCCESS) break;
        HKEY k;
        LSTATUS ls;
        if (f[0][0] == L'w') {
            uint64_t v = wcstoull(f[5], NULL, 16);
            ls = RegCreateKeyExW(root_key(f[1]), f[3], 0, NULL, 0, KEY_SET_VALUE | view_flag(f[2]), NULL, &k, NULL);
            if (ls == ERROR_SUCCESS) {
                ls = RegSetValueExW(k, f[4][0] ? f[4] : NULL, 0, REG_QWORD, (const BYTE *)&v, sizeof v);
                RegCloseKey(k);
            }
        } else {
            ls = RegOpenKeyExW(root_key(f[1]), f[3], 0, KEY_SET_VALUE | view_flag(f[2]), &k);
            if (ls == ERROR_SUCCESS) {
                ls = RegDeleteValueW(k, f[4][0] ? f[4] : NULL);
                RegCloseKey(k);
            }
            if (ls == ERROR_FILE_NOT_FOUND) ls = ERROR_SUCCESS;     // already gone
        }
        log_line(h, L"rubrapack: qword %ls %ls\\%ls\\%ls -> %ld", f[0][0] == L'w' ? L"write" : L"delete", f[1], f[3], f[4], (long)ls);
        if (ls != ERROR_SUCCESS && !rollback) rc = ERROR_INSTALL_FAILURE;
    }
    HeapFree(GetProcessHeap(), 0, data);
    return rollback ? ERROR_SUCCESS : rc;
}

__declspec(dllexport) UINT __stdcall RpQwordApply(MSIHANDLE h) { return run_list(h, false); }

__declspec(dllexport) UINT __stdcall RpQwordRollback(MSIHANDLE h) { return run_list(h, true); }

// ---- install folder guard ------------------------------------------------------------------------

static bool trusted_owner(PSID owner) {
    static const wchar_t *const sids[] = { L"S-1-5-18", L"S-1-5-32-544",
                                           L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464" };
    bool ok = false;
    for (size_t i = 0; i < sizeof sids / sizeof sids[0] && !ok; ++i) {
        PSID s = NULL;
        if (ConvertStringSidToSidW(sids[i], &s)) {
            ok = EqualSid(owner, s) != 0;
            LocalFree(s);
        }
    }
    return ok;
}

// 0: fine (absent, or present and owned by a trusted account), 1: refused.
static int check_dir(MSIHANDLE h, const wchar_t *path) {
    size_t n = wcslen(path);
    wchar_t *part = HeapAlloc(GetProcessHeap(), 0, (n + 1) * sizeof *part);
    if (part == NULL) return 1;
    // Every existing part of the path, from the one under the root down, is a real folder.
    size_t start = n >= 3 && path[1] == L':' ? 3 : 0;
    bool exists = true;
    for (size_t i = start; i <= n && exists; ++i) {
        if (i < n && path[i] != L'\\') continue;
        if (i == start) continue;
        memcpy(part, path, i * sizeof *part);
        part[i] = 0;
        DWORD a = GetFileAttributesW(part);
        if (a == INVALID_FILE_ATTRIBUTES) {
            exists = false;             // the rest does not exist either: the engine creates it
            break;
        }
        if ((a & FILE_ATTRIBUTE_REPARSE_POINT) || !(a & FILE_ATTRIBUTE_DIRECTORY)) {
            log_line(h, L"rubrapack: guard: %ls is a link or not a folder", part);
            HeapFree(GetProcessHeap(), 0, part);
            return 1;
        }
    }
    int rc = 0;
    if (exists) {
        PSID owner = NULL;
        PSECURITY_DESCRIPTOR sd = NULL;
        DWORD e = GetNamedSecurityInfoW(part, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &owner, NULL, NULL, NULL, &sd);
        if (e != ERROR_SUCCESS || owner == NULL || !trusted_owner(owner)) {
            wchar_t *text = NULL;
            if (e == ERROR_SUCCESS && owner) ConvertSidToStringSidW(owner, &text);
            log_line(h, L"rubrapack: guard: %ls exists and is owned by %ls", part, text ? text : L"(unreadable)");
            if (text) LocalFree(text);
            rc = 1;
        } else {
            log_line(h, L"rubrapack: guard: %ls exists, owned by a trusted account", part);
        }
        if (sd) LocalFree(sd);
    }
    HeapFree(GetProcessHeap(), 0, part);
    return rc;
}

__declspec(dllexport) UINT __stdcall RpGuardDirs(MSIHANDLE h) {
    wchar_t *list = get_property(h, L"RP_GUARD");
    if (list == NULL) return ERROR_INSTALL_FAILURE;
    UINT rc = ERROR_SUCCESS;
    wchar_t *ctx = NULL;
    for (wchar_t *dir = wcstok(list, L";", &ctx); dir && rc == ERROR_SUCCESS; dir = wcstok(NULL, L";", &ctx)) {
        wchar_t path[MAX_PATH * 4];
        DWORD n = sizeof path / sizeof path[0];
        if (MsiGetTargetPathW(h, dir, path, &n) != ERROR_SUCCESS) {
            log_line(h, L"rubrapack: guard: no target path for %ls", dir);
            rc = ERROR_INSTALL_FAILURE;
            break;
        }
        size_t len = wcslen(path);
        if (len > 3 && path[len - 1] == L'\\') path[len - 1] = 0;
        if (check_dir(h, path) == 0) continue;
        // The message in the chosen language, with [1] = the folder.
        wchar_t *lang = get_property(h, L"RPLANGUAGE");
        wchar_t name[64];
        swprintf(name, 64, L"RpGuardMsg_%ls", lang && lang[0] ? lang : L"en");
        wchar_t *msg = get_property(h, name);
        if (msg == NULL || msg[0] == 0) {
            if (msg) HeapFree(GetProcessHeap(), 0, msg);
            msg = get_property(h, L"RpGuardMsg_en");
        }
        MSIHANDLE rec = MsiCreateRecord(1);
        MsiRecordSetStringW(rec, 0, msg ? msg : L"[1]");
        MsiRecordSetStringW(rec, 1, path);
        MsiProcessMessage(h, (INSTALLMESSAGE)(INSTALLMESSAGE_ERROR | MB_OK | MB_ICONWARNING), rec);
        MsiCloseHandle(rec);
        if (msg) HeapFree(GetProcessHeap(), 0, msg);
        if (lang) HeapFree(GetProcessHeap(), 0, lang);
        rc = ERROR_INSTALL_FAILURE;
    }
    HeapFree(GetProcessHeap(), 0, list);
    return rc;
}

// Copies src to dst; src stays open for writing in Windows Installer, so it is shared both ways.
static bool copy_log(const wchar_t *src, const wchar_t *dst) {
    HANDLE in = CreateFileW(src, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (in == INVALID_HANDLE_VALUE) return false;
    HANDLE out = CreateFileW(dst, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (out == INVALID_HANDLE_VALUE) {
        CloseHandle(in);
        return false;
    }
    static char buf[1 << 16];
    bool ok = true;
    for (;;) {
        DWORD got = 0, put = 0;
        if (!ReadFile(in, buf, sizeof buf, &got, NULL)) {
            ok = false;
            break;
        }
        if (got == 0) break;
        if (!WriteFile(out, buf, got, &put, NULL) || put != got) {
            ok = false;
            break;
        }
    }
    CloseHandle(in);
    if (!CloseHandle(out)) ok = false;
    if (!ok) DeleteFileW(dst);
    return ok;
}

__declspec(dllexport) UINT __stdcall RpSaveLog(MSIHANDLE h) {
    wchar_t *log = get_property(h, L"MsiLogFileLocation");
    if (log == NULL || log[0] == 0) {
        if (log) HeapFree(GetProcessHeap(), 0, log);
        return ERROR_SUCCESS;
    }
    // The suggested name: the product's name, without the characters a file name cannot have.
    static wchar_t file[MAX_PATH * 4];
    wchar_t *product = get_property(h, L"ProductName");
    swprintf(file, MAX_PATH, L"%ls.log", product && product[0] ? product : L"setup");
    if (product) HeapFree(GetProcessHeap(), 0, product);
    for (wchar_t *p = file; *p; ++p) {
        if (*p < 32 || wcschr(L"\\/:*?\"<>|", *p)) *p = L'_';
    }
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = GetForegroundWindow();      // the setup's window, which waits for this action
    ofn.lpstrFilter = L"*.log\0*.log\0*.*\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = sizeof file / sizeof file[0];
    ofn.lpstrDefExt = L"log";
    ofn.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_HIDEREADONLY;
    if (GetSaveFileNameW(&ofn)) {
        if (copy_log(log, file)) {
            log_line(h, L"rubrapack: the log was saved to %ls", file);
        } else {
            wchar_t *lang = get_property(h, L"RPLANGUAGE");
            wchar_t name[64];
            swprintf(name, 64, L"RpLogMsg_%ls", lang && lang[0] ? lang : L"en");
            wchar_t *msg = get_property(h, name);
            if (msg == NULL || msg[0] == 0) {
                if (msg) HeapFree(GetProcessHeap(), 0, msg);
                msg = get_property(h, L"RpLogMsg_en");
            }
            MSIHANDLE rec = MsiCreateRecord(1);
            MsiRecordSetStringW(rec, 0, msg && msg[0] ? msg : L"[1]");
            MsiRecordSetStringW(rec, 1, file);
            MsiProcessMessage(h, (INSTALLMESSAGE)(INSTALLMESSAGE_ERROR | MB_OK | MB_ICONWARNING), rec);
            MsiCloseHandle(rec);
            if (msg) HeapFree(GetProcessHeap(), 0, msg);
            if (lang) HeapFree(GetProcessHeap(), 0, lang);
        }
    }
    HeapFree(GetProcessHeap(), 0, log);
    return ERROR_SUCCESS;
}

// ---- [remove] upgrade = false --------------------------------------------------------------------

// `name` matches `pat` (* any run, ? one character), ignoring case.
static bool wild(const wchar_t *pat, const wchar_t *name) {
    const wchar_t *star = NULL, *back = NULL;
    while (*name) {
        if (*pat == L'*') {
            star = pat++;
            back = name;
        } else if (*pat == L'?' || (*pat && CharUpperW((LPWSTR)(uintptr_t)(uint16_t)*pat) == CharUpperW((LPWSTR)(uintptr_t)(uint16_t)*name))) {
            ++pat;
            ++name;
        } else if (star) {
            pat = star + 1;
            name = ++back;
        } else {
            return false;
        }
    }
    while (*pat == L'*') ++pat;
    return *pat == 0;
}

static void w_rec(writer_t *w, const wchar_t *op, const wchar_t *a, const wchar_t *b) {
    w_field(w, op);
    w_field(w, a);
    w_field(w, b);
}

// The cleanup folder of this product: %ProgramData% (per machine) or the user's %LOCALAPPDATA%
// (per user), then rubrapack\cleanup\<ProductCode>.
static bool cleanup_folder(MSIHANDLE h, bool machine, wchar_t out[MAX_PATH * 4]) {
    wchar_t base[MAX_PATH * 2];
    DWORD n = GetEnvironmentVariableW(machine ? L"ProgramData" : L"LOCALAPPDATA", base, MAX_PATH * 2);
    wchar_t *pc = get_property(h, L"ProductCode");
    bool ok = n > 0 && n < MAX_PATH * 2 && pc && pc[0];
    if (ok) swprintf(out, MAX_PATH * 4, L"%ls\\rubrapack\\cleanup\\%ls", base, pc);
    if (pc) HeapFree(GetProcessHeap(), 0, pc);
    return ok;
}

static void make_dirs(const wchar_t *path) {
    wchar_t part[MAX_PATH * 4];
    size_t n = wcslen(path);
    for (size_t i = 3; i <= n && i < MAX_PATH * 4; ++i) {
        if (i < n && path[i] != L'\\') continue;
        memcpy(part, path, i * sizeof *part);
        part[i] = 0;
        CreateDirectoryW(part, NULL);
    }
}

// Appends "file<TAB>path" to <folder>\pending.txt (UTF-16LE).
static void note_pending(MSIHANDLE h, const wchar_t *folder, const wchar_t *path) {
    wchar_t file[MAX_PATH * 4], line[MAX_PATH * 4 + 16];
    make_dirs(folder);
    swprintf(file, MAX_PATH * 4, L"%ls\\pending.txt", folder);
    HANDLE f = CreateFileW(file, FILE_APPEND_DATA, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    int n = swprintf(line, MAX_PATH * 4 + 16, L"file\t%ls\r\n", path);
    DWORD put = 0;
    if (n > 0) WriteFile(f, line, (DWORD)n * sizeof(wchar_t), &put, NULL);
    CloseHandle(f);
    log_line(h, L"rubrapack: remove: %ls noted for the cleanup task", path);
}

__declspec(dllexport) UINT __stdcall RpRemovePrepare(MSIHANDLE h) {
    wchar_t *plan = get_property(h, L"RP_REMOVES");
    wchar_t *root = get_property(h, L"RP_REMOVES_ROOT");
    wchar_t *upgrading = get_property(h, L"UPGRADINGPRODUCTCODE");
    writer_t apply = { 0 }, rollback = { 0 }, commit = { 0 };
    w_raw(&apply, L"RPR1", 4);
    w_raw(&rollback, L"RPR1", 4);
    w_raw(&commit, L"RPR1", 4);
    UINT rc = ERROR_SUCCESS;
    if (plan == NULL || root == NULL || upgrading == NULL || wcsncmp(plan, L"RPR1", 4) != 0) rc = ERROR_INSTALL_FAILURE;
    if (rc == ERROR_SUCCESS && upgrading[0]) {
        log_line(h, L"rubrapack: remove: an upgrade (%ls) removes this version: upgrade = false keeps the files", upgrading);
    } else if (rc == ERROR_SUCCESS) {
        // One backup folder per volume (or the temporary folder), named once per run.
        wchar_t tag[40];
        swprintf(tag, 40, L"rp-%08lX%08lX", (unsigned long)GetCurrentProcessId(), (unsigned long)GetTickCount());
        // Where a file that cannot even be queued for the next restart (per user) is noted.
        static wchar_t cfolder[MAX_PATH * 4];
        if (cleanup_folder(h, wcscmp(root, L"volume") == 0, cfolder)) w_rec(&apply, L"c", cfolder, L"");
        reader_t r = { plan + 4, plan + wcslen(plan) };
        static wchar_t f[3][MAX_TEXT / 8];
        static wchar_t dir[MAX_PATH * 4], src[MAX_PATH * 4], dst[MAX_PATH * 4], vol[MAX_PATH * 4], bdir[MAX_PATH * 4];
        static wchar_t dirs[8][MAX_PATH * 4];
        size_t ndirs = 0, n = 0;
        while (r.p < r.end) {
            if (!r_field(&r, f[0], MAX_TEXT / 8) || !r_field(&r, f[1], MAX_TEXT / 8) || !r_field(&r, f[2], MAX_TEXT / 8)) {
                rc = ERROR_INSTALL_FAILURE;
                break;
            }
            INSTALLSTATE installed = INSTALLSTATE_UNKNOWN, action = INSTALLSTATE_UNKNOWN;
            MsiGetComponentStateW(h, f[2], &installed, &action);
            if (action != INSTALLSTATE_ABSENT) continue;
            DWORD dn = MAX_PATH * 4;
            if (MsiGetTargetPathW(h, f[0], dir, &dn) != ERROR_SUCCESS) continue;
            size_t dl = wcslen(dir);
            if (f[1][0] == 0) {                 // the folder itself, once the engine has emptied it
                if (dl > 3 && dir[dl - 1] == L'\\') dir[dl - 1] = 0;
                w_rec(&commit, L"r", dir, L"");
                continue;
            }
            swprintf(src, MAX_PATH * 4, L"%ls*", dir);
            WIN32_FIND_DATAW fd;
            HANDLE fh = FindFirstFileExW(src, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0);
            if (fh == INVALID_HANDLE_VALUE) continue;
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                if (!wild(f[1], fd.cFileName)) continue;
                swprintf(src, MAX_PATH * 4, L"%ls%ls", dir, fd.cFileName);
                if (wcscmp(root, L"volume") == 0 && GetVolumePathNameW(src, vol, MAX_PATH * 4)) {
                    swprintf(bdir, MAX_PATH * 4, L"%lsConfig.Msi\\%ls", vol, tag);
                } else {
                    DWORD tn = GetTempPathW(MAX_PATH * 4, vol);
                    if (tn == 0 || tn >= MAX_PATH * 4) continue;
                    swprintf(bdir, MAX_PATH * 4, L"%ls%ls", vol, tag);
                }
                swprintf(dst, MAX_PATH * 4, L"%ls\\%zu", bdir, n++);
                w_rec(&apply, L"m", src, dst);
                w_rec(&rollback, L"m", dst, src);
                w_rec(&commit, L"f", dst, L"");
                bool seen = false;
                for (size_t i = 0; i < ndirs; ++i) seen = seen || wcscmp(dirs[i], bdir) == 0;
                if (!seen && ndirs < 8) wcscpy(dirs[ndirs++], bdir);
                log_line(h, L"rubrapack: remove: %ls", src);
            } while (FindNextFileW(fh, &fd));
            FindClose(fh);
        }
        // The backup folders go last: after the files in them (commit), after moving back (rollback).
        for (size_t i = 0; i < ndirs; ++i) {
            w_rec(&apply, L"d", dirs[i], L"");
            w_rec(&rollback, L"x", dirs[i], L"");
            w_rec(&commit, L"x", dirs[i], L"");
        }
    }
    if (rc == ERROR_SUCCESS) rc = set_data(h, L"RP_RemoveApply", &apply);
    if (rc == ERROR_SUCCESS) rc = set_data(h, L"RP_RemoveApplyRollback", &rollback);
    if (rc == ERROR_SUCCESS) rc = set_data(h, L"RP_RemoveCommit", &commit);
    if (plan) HeapFree(GetProcessHeap(), 0, plan);
    if (root) HeapFree(GetProcessHeap(), 0, root);
    if (upgrading) HeapFree(GetProcessHeap(), 0, upgrading);
    if (apply.buf) HeapFree(GetProcessHeap(), 0, apply.buf);
    if (rollback.buf) HeapFree(GetProcessHeap(), 0, rollback.buf);
    if (commit.buf) HeapFree(GetProcessHeap(), 0, commit.buf);
    return rc;
}

// A backup folder only SYSTEM and Administrators can open (it holds removed files for a moment).
static void make_private_dir(MSIHANDLE h, const wchar_t *path) {
    wchar_t parent[MAX_PATH * 4];
    wcsncpy(parent, path, MAX_PATH * 4 - 1);
    parent[MAX_PATH * 4 - 1] = 0;
    wchar_t *slash = wcsrchr(parent, L'\\');
    if (slash) {
        *slash = 0;
        CreateDirectoryW(parent, NULL);      // Config.Msi, normally there already
    }
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, FALSE };
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)", SDDL_REVISION_1,
                                                             &sa.lpSecurityDescriptor, NULL)) {
        if (!CreateDirectoryW(path, &sa) && GetLastError() != ERROR_ALREADY_EXISTS) {
            CreateDirectoryW(path, NULL);    // a per-user installation cannot set that owner list
        }
        LocalFree(sa.lpSecurityDescriptor);
    } else {
        CreateDirectoryW(path, NULL);
    }
    log_line(h, L"rubrapack: remove: backup folder %ls", path);
}

// Runs a list made by RpRemovePrepare: m move (src, dst), d make the backup folder, f delete a
// backup file, x delete a backup folder, r delete a folder of the package if empty.
static void remove_list(MSIHANDLE h, bool to_backup) {
    wchar_t *data = get_property(h, L"CustomActionData");
    if (data == NULL) return;
    static wchar_t note[MAX_PATH * 4];
    note[0] = 0;
    if (wcsncmp(data, L"RPR1", 4) == 0) {
        reader_t r = { data + 4, data + wcslen(data) };
        static wchar_t f[3][MAX_PATH * 4];
        bool made = false;
        while (r.p < r.end && r_field(&r, f[0], 8) && r_field(&r, f[1], MAX_PATH * 4) && r_field(&r, f[2], MAX_PATH * 4)) {
            wchar_t op = f[0][0];
            if (op == L'd') {                // apply lists its folders last: make them on the first move instead
                continue;
            }
            if (op == L'c') {
                wcscpy(note, f[1]);
                continue;
            }
            if (op == L'm') {
                if (to_backup && !made) {
                    // Every backup folder named in this list, before the first move.
                    reader_t s = { data + 4, data + wcslen(data) };
                    static wchar_t g[3][MAX_PATH * 4];
                    while (s.p < s.end && r_field(&s, g[0], 8) && r_field(&s, g[1], MAX_PATH * 4) && r_field(&s, g[2], MAX_PATH * 4)) {
                        if (g[0][0] == L'd') make_private_dir(h, g[1]);
                    }
                    made = true;
                }
                BOOL ok = MoveFileExW(f[1], f[2], MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH);
                DWORD e = ok ? 0 : GetLastError();
                log_line(h, L"rubrapack: remove: move %ls -> %ls: %lu", f[1], f[2], (unsigned long)e);
                if (!ok && to_backup) {          // held: it goes at the next restart instead
                    BOOL later = MoveFileExW(f[1], NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
                    log_line(h, L"rubrapack: remove: %ls at the next restart: %ls", f[1], later ? L"queued" : L"could not queue");
                    if (!later && note[0]) note_pending(h, note, f[1]);     // per user: the cleanup task's job
                }
            } else if (op == L'f') {
                if (!DeleteFileW(f[1]) && GetLastError() != ERROR_FILE_NOT_FOUND) MoveFileExW(f[1], NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
            } else if (op == L'x') {
                if (!RemoveDirectoryW(f[1]) && GetLastError() != ERROR_FILE_NOT_FOUND) MoveFileExW(f[1], NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
            } else if (op == L'r') {
                BOOL ok = RemoveDirectoryW(f[1]);
                log_line(h, L"rubrapack: remove: folder %ls: %ls", f[1], ok ? L"removed" : L"kept (not empty or not there)");
            }
        }
    }
    HeapFree(GetProcessHeap(), 0, data);
}

__declspec(dllexport) UINT __stdcall RpRemoveApply(MSIHANDLE h) {
    remove_list(h, true);
    return ERROR_SUCCESS;
}

__declspec(dllexport) UINT __stdcall RpRemoveRollback(MSIHANDLE h) {
    remove_list(h, false);
    return ERROR_SUCCESS;
}

__declspec(dllexport) UINT __stdcall RpRemoveCommit(MSIHANDLE h) {
    remove_list(h, false);
    return ERROR_SUCCESS;
}

// ---- cleaning up later (RFC-0026) -----------------------------------------------------------------

// Rescue (RFC-0026, x41): a file this installation installs - or keeps, because an identical one is
// already there - may carry a deletion an earlier removal queued for the next restart; the restart
// would then delete this installation's file. Per machine only (the list is in HKLM).
//   RpRescuePrepare immediate: the files of components being installed whose path has a queued
//                   deletion now, with how many renames are queued now (later ones are this
//                   installation's own and stay).
//   RpRescueApply   deferred, after InstallFiles: takes those deletions out.
//   RpRescueRollback deferred rollback: queues them again.

__declspec(dllexport) UINT __stdcall RpRescuePrepare(MSIHANDLE h) {
    writer_t w = { 0 };
    w_raw(&w, L"RPS1", 4);
    // The queued deletions now.
    HKEY k;
    wchar_t *pend = NULL;
    DWORD size = 0;
    unsigned long count = 0;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, PFRO_KEY, 0, KEY_QUERY_VALUE, &k) == ERROR_SUCCESS) {
        pend = pfro_read(k, &size);
        RegCloseKey(k);
    }
    for (wchar_t *src = pend; src && *src; ++count) {
        wchar_t *dst = src + wcslen(src) + 1;
        src = *dst ? dst + wcslen(dst) + 1 : dst + 1;
    }
    wchar_t num[24];
    swprintf(num, 24, L"%lu", count);
    w_field(&w, num);
    MSIHANDLE db = pend ? MsiGetActiveDatabase(h) : 0, view = 0, rec = 0;
    size_t n = 0;
    if (db && MsiDatabaseOpenViewW(db, L"SELECT `File`.`FileName`, `Component`.`Directory_`, `Component`.`Component` FROM `File`, `Component` "
                                       L"WHERE `File`.`Component_` = `Component`.`Component`", &view) == ERROR_SUCCESS &&
        MsiViewExecute(view, 0) == ERROR_SUCCESS) {
        static wchar_t fname[1024], dir[80], comp[80], path[MAX_PATH * 4];
        while (MsiViewFetch(view, &rec) == ERROR_SUCCESS) {
            DWORD fn = 1024, dn = 80, cn = 80, pn = MAX_PATH * 4;
            INSTALLSTATE installed = INSTALLSTATE_UNKNOWN, action = INSTALLSTATE_UNKNOWN;
            if (MsiRecordGetStringW(rec, 1, fname, &fn) == ERROR_SUCCESS && MsiRecordGetStringW(rec, 2, dir, &dn) == ERROR_SUCCESS &&
                MsiRecordGetStringW(rec, 3, comp, &cn) == ERROR_SUCCESS && MsiGetComponentStateW(h, comp, &installed, &action) == ERROR_SUCCESS &&
                action == INSTALLSTATE_LOCAL && MsiGetTargetPathW(h, dir, path, &pn) == ERROR_SUCCESS) {
                wchar_t *bar = wcschr(fname, L'|');
                size_t len = wcslen(path);
                if (len + wcslen(bar ? bar + 1 : fname) < MAX_PATH * 4) wcscpy(path + len, bar ? bar + 1 : fname);
                bool queued = false;
                for (wchar_t *src = pend; *src && !queued;) {
                    wchar_t *dst = src + wcslen(src) + 1;
                    queued = *dst == 0 && CompareStringOrdinal(pfro_path(src), -1, path, -1, TRUE) == CSTR_EQUAL;
                    src = *dst ? dst + wcslen(dst) + 1 : dst + 1;
                }
                if (queued) {
                    w_field(&w, path);
                    ++n;
                    log_line(h, L"rubrapack: rescue: %ls has a deletion queued for the next restart", path);
                }
            }
            MsiCloseHandle(rec);
        }
    }
    if (view) MsiCloseHandle(view);
    if (db) MsiCloseHandle(db);
    if (pend) HeapFree(GetProcessHeap(), 0, pend);
    if (n == 0) {                       // nothing to do: empty lists
        w.len = 0;
        w_raw(&w, L"RPS1", 4);
        w_field(&w, num);
    }
    set_data(h, L"RP_RescueApply", &w);
    set_data(h, L"RP_RescueApplyRollback", &w);
    if (w.buf) HeapFree(GetProcessHeap(), 0, w.buf);
    return ERROR_SUCCESS;
}

static void rescue_list(MSIHANDLE h, bool undo) {
    wchar_t *data = get_property(h, L"CustomActionData");
    if (data == NULL) return;
    reader_t r = { data, data + wcslen(data) };
    static wchar_t num[24], path[MAX_PATH * 4];
    if (wcsncmp(data, L"RPS1", 4) == 0) {
        r.p += 4;
        if (r_field(&r, num, 24)) {
            unsigned long before = wcstoul(num, NULL, 10);
            while (r_field(&r, path, MAX_PATH * 4)) {
                if (undo) {
                    BOOL ok = MoveFileExW(path, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
                    log_line(h, L"rubrapack: rescue: %ls queued again: %ls", path, ok ? L"yes" : L"no");
                } else {
                    int taken = pfro_cancel(path, before);
                    log_line(h, L"rubrapack: rescue: %ls: %d queued deletion(s) taken back", path, taken);
                }
            }
        }
    }
    HeapFree(GetProcessHeap(), 0, data);
}

__declspec(dllexport) UINT __stdcall RpRescueApply(MSIHANDLE h) {
    rescue_list(h, false);
    return ERROR_SUCCESS;
}

__declspec(dllexport) UINT __stdcall RpRescueRollback(MSIHANDLE h) {
    rescue_list(h, true);
    return ERROR_SUCCESS;
}

static unsigned long queued_renames(void);

__declspec(dllexport) UINT __stdcall RpCleanupPrepare(MSIHANDLE h) {
    wchar_t *dirs = get_property(h, L"RP_CLEANUP_DIRS");
    wchar_t *scope = get_property(h, L"RP_CLEANUP_SCOPE");
    wchar_t *pc = get_property(h, L"ProductCode");
    writer_t w = { 0 };
    w_raw(&w, L"RPC1", 4);
    static wchar_t folder[MAX_PATH * 4], path[MAX_PATH * 4];
    bool machine = scope && wcscmp(scope, L"machine") == 0;
    // Why this run may leave something. Windows Installer queues the installer's backups for the
    // next restart only after the commit actions (x40), so the commit cannot know - it registers the
    // task for every run that removes or replaces files, and the task's first run, two minutes
    // later, sees what there is (and removes itself when there is nothing).
    wchar_t *remove = get_property(h, L"REMOVE"), *older = get_property(h, L"RP_OLDER_FOUND"), *installed = get_property(h, L"Installed");
    const wchar_t *why = remove && remove[0] ? L"remove" : older && older[0] ? L"upgrade" : installed && installed[0] ? L"maintenance" : L"";
    if (dirs && pc && cleanup_folder(h, machine, folder)) {
        w_field(&w, pc);
        w_field(&w, machine ? L"machine" : L"user");
        w_field(&w, folder);
        w_field(&w, why);
        // The renames queued before this installation's script runs: what is queued after them is
        // this installation's (a held file's deletion during the script, the installer's backups
        // at its end - x40). Counted here, in the immediate pass, before any of it happens.
        wchar_t count[24];
        swprintf(count, 24, L"%lu", queued_renames());
        w_field(&w, count);
        wchar_t *ctx = NULL;
        for (wchar_t *d = wcstok(dirs, L";", &ctx); d; d = wcstok(NULL, L";", &ctx)) {
            DWORD n = MAX_PATH * 4;
            if (MsiGetTargetPathW(h, d, path, &n) != ERROR_SUCCESS) continue;
            size_t len = wcslen(path);
            if (len > 3 && path[len - 1] == L'\\') path[len - 1] = 0;
            w_field(&w, path);
        }
    }
    set_data(h, L"RP_CleanupRegister", &w);
    if (remove) HeapFree(GetProcessHeap(), 0, remove);
    if (older) HeapFree(GetProcessHeap(), 0, older);
    if (installed) HeapFree(GetProcessHeap(), 0, installed);
    if (dirs) HeapFree(GetProcessHeap(), 0, dirs);
    if (scope) HeapFree(GetProcessHeap(), 0, scope);
    if (pc) HeapFree(GetProcessHeap(), 0, pc);
    if (w.buf) HeapFree(GetProcessHeap(), 0, w.buf);
    return ERROR_SUCCESS;
}

// How many renames Windows has queued for the next restart (PendingFileRenameOperations pairs).
static unsigned long queued_renames(void) {
    HKEY k;
    unsigned long n = 0;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager", 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS) return 0;
    DWORD type = 0, size = 0;
    if (RegQueryValueExW(k, L"PendingFileRenameOperations", NULL, &type, NULL, &size) == ERROR_SUCCESS && type == REG_MULTI_SZ && size) {
        wchar_t *buf = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size + 2 * sizeof(wchar_t));
        if (buf && RegQueryValueExW(k, L"PendingFileRenameOperations", NULL, &type, (BYTE *)buf, &size) == ERROR_SUCCESS) {
            for (wchar_t *src = buf; *src; ++n) {
                wchar_t *dst = src + wcslen(src) + 1;
                src = *dst ? dst + wcslen(dst) + 1 : dst + 1;
            }
        }
        if (buf) HeapFree(GetProcessHeap(), 0, buf);
    }
    RegCloseKey(k);
    return n;
}

// Appends text to a growing buffer (writer_t without the field lengths).
static void w_text(writer_t *w, const wchar_t *s) { w_raw(w, s, wcslen(s)); }

static void w_xml(writer_t *w, const wchar_t *s) {
    for (; *s; ++s) {
        if (*s == L'&') w_text(w, L"&amp;");
        else if (*s == L'<') w_text(w, L"&lt;");
        else if (*s == L'>') w_text(w, L"&gt;");
        else if (*s == L'"') w_text(w, L"&quot;");
        else w_raw(w, s, 1);
    }
}

static bool write_file(const wchar_t *path, const void *data, size_t len, const SECURITY_ATTRIBUTES *sa) {
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, (SECURITY_ATTRIBUTES *)sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    bool ok = WriteFile(f, data, (DWORD)len, &put, NULL) && put == len;
    if (!CloseHandle(f)) ok = false;
    return ok;
}

static DWORD run_wait(MSIHANDLE h, wchar_t *cmd) {
    STARTUPINFOW si = { .cb = sizeof si };
    PROCESS_INFORMATION pi;
    DWORD code = (DWORD)-1;
    if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 60000);
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    log_line(h, L"rubrapack: cleanup: %ls -> %ld", cmd, (long)code);
    return code;
}

// Local time `minutes` from now: "YYYY-MM-DDTHH:MM:SS" and "YYYYMMDD".
static void when_minutes(long long minutes, wchar_t iso[24], wchar_t ymd[12]) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u = { .LowPart = ft.dwLowDateTime, .HighPart = ft.dwHighDateTime };
    u.QuadPart += (unsigned long long)minutes * 600000000ULL;
    ft.dwLowDateTime = u.LowPart;
    ft.dwHighDateTime = u.HighPart;
    FILETIME lt;
    SYSTEMTIME st;
    FileTimeToLocalFileTime(&ft, &lt);
    FileTimeToSystemTime(&lt, &st);
    swprintf(iso, 24, L"%04u-%02u-%02uT%02u:%02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    swprintf(ymd, 12, L"%04u%02u%02u", st.wYear, st.wMonth, st.wDay);
}

static void when(int days, wchar_t iso[24], wchar_t ymd[12]) { when_minutes((long long)days * 1440, iso, ymd); }

__declspec(dllexport) UINT __stdcall RpCleanupRegister(MSIHANDLE h) {
    wchar_t *data = get_property(h, L"CustomActionData");
    if (data == NULL) return ERROR_SUCCESS;
    reader_t r = { data, data + wcslen(data) };
    static wchar_t pc[64], scope[16], folder[MAX_PATH * 4], why[16], dirs[64][MAX_PATH * 4];
    size_t ndirs = 0;
    if (wcsncmp(data, L"RPC1", 4) != 0) goto done;
    r.p += 4;
    static wchar_t count[24];
    if (!r_field(&r, pc, 64) || !r_field(&r, scope, 16) || !r_field(&r, folder, MAX_PATH * 4) || !r_field(&r, why, 16) ||
        !r_field(&r, count, 24))
        goto done;
    while (ndirs < 64 && r_field(&r, dirs[ndirs], MAX_PATH * 4)) ++ndirs;
    bool machine = wcscmp(scope, L"machine") == 0;
    writer_t list = { 0 };
    size_t found = 0;
    // Files a per-user package could not queue (RpRemoveApply wrote them down).
    static wchar_t pend[MAX_PATH * 4];
    swprintf(pend, MAX_PATH * 4, L"%ls\\pending.txt", folder);
    writer_t files = { 0 };
    HANDLE pf = CreateFileW(pend, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (pf != INVALID_HANDLE_VALUE) {
        DWORD size = GetFileSize(pf, NULL), got = 0;
        if (size != INVALID_FILE_SIZE && size < (1u << 20) && size % 2 == 0) {
            wchar_t *t = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size + sizeof(wchar_t));
            if (t && ReadFile(pf, t, size, &got, NULL) && got == size) {
                wchar_t *ctx = NULL;
                for (wchar_t *line = wcstok(t, L"\r\n", &ctx); line; line = wcstok(NULL, L"\r\n", &ctx)) {
                    if (wcsncmp(line, L"file\t", 5) != 0) continue;
                    w_text(&files, line);
                    w_text(&files, L"\r\n");
                    ++found;
                }
            }
            if (t) HeapFree(GetProcessHeap(), 0, t);
        }
        CloseHandle(pf);
    }
    log_line(h, L"rubrapack: cleanup: run kind '%ls', %zu file(s) noted", why, found);
    if ((why[0] == 0 && found == 0) || rp_clean_part_len < 2) {
        if (rp_clean_part_len < 2) log_line(h, L"rubrapack: cleanup: this helper was built without rubrapack_clean.exe");
        if (files.buf) HeapFree(GetProcessHeap(), 0, files.buf);
        goto done;
    }
    static wchar_t name[160], iso0[24], iso1[24], ymd[12], ymd0[12], path[MAX_PATH * 4], exe[MAX_PATH * 4], cmd[MAX_PATH * 8], sys[MAX_PATH];
    // The task, from an XML file: SYSTEM (per machine) or this user, at logon and every 15 minutes
    // for 30 days; Windows deletes it when the time is up.
    wchar_t *sid = NULL;
    if (!machine) {
        HANDLE tok = NULL;
        static BYTE tu[256];
        DWORD n = 0;
        if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &tok) || OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
            if (GetTokenInformation(tok, TokenUser, tu, sizeof tu, &n)) ConvertSidToStringSidW(((TOKEN_USER *)tu)->User.Sid, &sid);
            CloseHandle(tok);
        }
    }
    // Per user the name carries the user's SID: several users may each install the same product.
    if (machine || sid == NULL) swprintf(name, 160, L"rubrapack cleanup %ls", pc);
    else swprintf(name, 160, L"rubrapack cleanup %ls %ls", pc, sid);
    when(0, iso0, ymd0);
    static wchar_t isofirst[24], ymdfirst[12];
    when_minutes(2, isofirst, ymdfirst);
    when(30, iso1, ymd);
    w_raw(&list, L"\xFEFF" L"RPC1\r\n", 7);
    w_text(&list, L"task\t"); w_text(&list, name); w_text(&list, L"\r\n");
    w_text(&list, L"until\t"); w_text(&list, ymd); w_text(&list, L"\r\n");
    w_text(&list, L"product\t"); w_text(&list, pc); w_text(&list, L"\r\n");
    w_text(&list, L"scope\t"); w_text(&list, scope); w_text(&list, L"\r\n");
    w_text(&list, L"kind\t"); w_text(&list, why); w_text(&list, L"\r\n");
    w_text(&list, L"after\t"); w_text(&list, count); w_text(&list, L"\r\n");
    for (size_t i = 0; i < ndirs; ++i) {
        w_text(&list, L"root\t"); w_text(&list, dirs[i]); w_text(&list, L"\r\n");
    }
    if (files.buf) w_text(&list, files.buf);
    // The folder: per machine only SYSTEM and Administrators may write (the task runs as SYSTEM), and
    // it must not exist already with another owner or pass through a link (%ProgramData% lets
    // users create folders: one planted there is refused, with the guard's own check).
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, FALSE };
    if (machine) {
        static wchar_t base[MAX_PATH * 4];
        wcscpy(base, folder);
        for (int up = 0; up < 2; ++up) {
            wchar_t *slash = wcsrchr(base, L'\\');
            if (slash) *slash = 0;
        }
        if (check_dir(h, folder) != 0) {
            log_line(h, L"rubrapack: cleanup: %ls is not safe to use; nothing registered", folder);
            if (files.buf) HeapFree(GetProcessHeap(), 0, files.buf);
            if (list.buf) HeapFree(GetProcessHeap(), 0, list.buf);
            goto done;
        }
        ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;GRGX;;;BU)", SDDL_REVISION_1,
                                                             &sa.lpSecurityDescriptor, NULL);
        make_dirs(folder);
        if (sa.lpSecurityDescriptor) {
            SetFileSecurityW(base, DACL_SECURITY_INFORMATION, sa.lpSecurityDescriptor);     // %ProgramData%\rubrapack
            SetFileSecurityW(folder, DACL_SECURITY_INFORMATION, sa.lpSecurityDescriptor);
        }
        if (check_dir(h, folder) != 0) {            // made by someone else in between
            log_line(h, L"rubrapack: cleanup: %ls is not safe to use; nothing registered", folder);
            if (files.buf) HeapFree(GetProcessHeap(), 0, files.buf);
            if (list.buf) HeapFree(GetProcessHeap(), 0, list.buf);
            if (sa.lpSecurityDescriptor) LocalFree(sa.lpSecurityDescriptor);
            goto done;
        }
    } else {
        make_dirs(folder);
    }
    swprintf(exe, MAX_PATH * 4, L"%ls\\rubrapack_clean.exe", folder);
    swprintf(path, MAX_PATH * 4, L"%ls\\list.txt", folder);
    bool ok = write_file(exe, rp_clean_part, rp_clean_part_len, sa.lpSecurityDescriptor ? &sa : NULL) &&
              write_file(path, list.buf, list.len * sizeof(wchar_t), sa.lpSecurityDescriptor ? &sa : NULL);
    writer_t x = { 0 };
    w_raw(&x, L"\xFEFF", 1);
    w_text(&x, L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n");
    w_text(&x, L"<RegistrationInfo><Description>rubrapack: deletes what the installation of ");
    w_xml(&x, pc);
    w_text(&x, L" had to leave until the next restart, as soon as nothing holds it.</Description></RegistrationInfo>\r\n<Triggers>\r\n<LogonTrigger><StartBoundary>");
    w_text(&x, iso0); w_text(&x, L"</StartBoundary><EndBoundary>"); w_text(&x, iso1); w_text(&x, L"</EndBoundary><Enabled>true</Enabled>");
    if (sid) { w_text(&x, L"<UserId>"); w_text(&x, sid); w_text(&x, L"</UserId>"); }
    w_text(&x, L"</LogonTrigger>\r\n<TimeTrigger><Repetition><Interval>PT15M</Interval><StopAtDurationEnd>false</StopAtDurationEnd></Repetition><StartBoundary>");
    w_text(&x, isofirst); w_text(&x, L"</StartBoundary><EndBoundary>"); w_text(&x, iso1); w_text(&x, L"</EndBoundary><Enabled>true</Enabled></TimeTrigger>\r\n</Triggers>\r\n");
    w_text(&x, L"<Principals><Principal id=\"Author\"><UserId>");
    w_text(&x, machine ? L"S-1-5-18" : sid ? sid : L"");
    // HighestAvailable for a user too: a task an elevated installation registered can be deleted
    // only with the same rights (x40), and the task deletes itself at the end.
    w_text(&x, machine ? L"</UserId><RunLevel>HighestAvailable</RunLevel>" : L"</UserId><LogonType>InteractiveToken</LogonType><RunLevel>HighestAvailable</RunLevel>");
    w_text(&x, L"</Principal></Principals>\r\n<Settings><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy><DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>"
               L"<StopIfGoingOnBatteries>false</StopIfGoingOnBatteries><StartWhenAvailable>true</StartWhenAvailable><ExecutionTimeLimit>PT10M</ExecutionTimeLimit>"
               L"<DeleteExpiredTaskAfter>PT0S</DeleteExpiredTaskAfter></Settings>\r\n<Actions Context=\"Author\"><Exec><Command>");
    w_xml(&x, exe);
    w_text(&x, L"</Command><Arguments>\"");
    w_xml(&x, folder);
    w_text(&x, L"\"</Arguments></Exec></Actions>\r\n</Task>\r\n");
    if (sid) LocalFree(sid);
    swprintf(path, MAX_PATH * 4, L"%ls\\task.xml", folder);
    ok = ok && !x.bad && !list.bad && write_file(path, x.buf, x.len * sizeof(wchar_t), NULL);
    GetSystemDirectoryW(sys, MAX_PATH);
    if (ok) {
        swprintf(cmd, MAX_PATH * 8, L"\"%ls\\schtasks.exe\" /create /tn \"%ls\" /xml \"%ls\" /f", sys, name, path);
        run_wait(h, cmd);     // first run two minutes from now: the engine's own deletions are queued by then
    } else {
        log_line(h, L"rubrapack: cleanup: could not write %ls", folder);
    }
    DeleteFileW(path);
    DeleteFileW(pend);
    if (sa.lpSecurityDescriptor) LocalFree(sa.lpSecurityDescriptor);
    if (x.buf) HeapFree(GetProcessHeap(), 0, x.buf);
    if (list.buf) HeapFree(GetProcessHeap(), 0, list.buf);
    if (files.buf) HeapFree(GetProcessHeap(), 0, files.buf);
done:
    HeapFree(GetProcessHeap(), 0, data);
    return ERROR_SUCCESS;
}
