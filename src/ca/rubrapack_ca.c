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
    if (wcsncmp(data, L"RPR1", 4) == 0) {
        reader_t r = { data + 4, data + wcslen(data) };
        static wchar_t f[3][MAX_PATH * 4];
        bool made = false;
        while (r.p < r.end && r_field(&r, f[0], 8) && r_field(&r, f[1], MAX_PATH * 4) && r_field(&r, f[2], MAX_PATH * 4)) {
            wchar_t op = f[0][0];
            if (op == L'd') {                // apply lists its folders last: make them on the first move instead
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
