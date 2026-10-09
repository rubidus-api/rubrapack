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
// Add-ons (x46): with RP_REMOVE_ADDONS = 1 ([package] remove-addons), a real removal
//   (REMOVE=ALL, not inside an upgrade) has RpCleanupPrepare list the installed products named under
//   SOFTWARE\rubrapack\Addons\<UpgradeCode> (HKLM per machine, HKCU per user; both registry views),
//   which add-ons write there ([package] parent). RpCleanupRegister writes them into the cleanup list
//   ("parent", "addon") and lets the task start at once; rubrapack_clean.exe removes them with
//   msiexec /x once this installation has ended.
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
#include <shlobj.h>
#include <commdlg.h>

#include "../clean/pfro.h"
#include "rp_appx.h"

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

// "{8-4-4-4-12}" in hex digits: what a ProductCode or UpgradeCode looks like. A code goes into
// paths, a task's name and a command line, so anything else is refused.
static bool is_guid(const wchar_t *s) {
    if (s == NULL || wcslen(s) != 38 || s[0] != L'{' || s[37] != L'}') return false;
    for (int i = 1; i < 37; ++i) {
        bool dash = i == 9 || i == 14 || i == 19 || i == 24;
        wchar_t c = s[i];
        bool hex = (c >= L'0' && c <= L'9') || (c >= L'A' && c <= L'F') || (c >= L'a' && c <= L'f');
        if (dash ? c != L'-' : !hex) return false;
    }
    return true;
}

static void log_line(MSIHANDLE h, const wchar_t *fmt, ...) {
    wchar_t text[1024];
    va_list ap;
    va_start(ap, fmt);
    text[0] = 0;
    vswprintf(text, 1024, fmt, ap);
    text[1023] = 0;                 // a line too long is cut, never left without its end
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
    if (plan == NULL || allusers == NULL) {
        if (plan) HeapFree(GetProcessHeap(), 0, plan);
        if (allusers) HeapFree(GetProcessHeap(), 0, allusers);
        return ERROR_INSTALL_FAILURE;
    }
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
        wfmt(oldhex, 24, L"%016llX", (unsigned long long)old);
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

// 0: fine (present and owned by a trusted account; or absent - then, with `above`, the deepest
// folder that does exist above it must be owned by one: a folder made inside a user's folder gets
// that folder's access list, and its owner can put another folder in its place), 1: refused.
static int check_dir(MSIHANDLE h, const wchar_t *path, bool above) {
    size_t n = wcslen(path);
    wchar_t *part = HeapAlloc(GetProcessHeap(), 0, (n + 1) * sizeof *part);
    if (part == NULL) return 1;
    // Every existing part of the path, from the one under the root down, is a real folder.
    size_t start = n >= 3 && path[1] == L':' ? 3 : 0, last = start;
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
        last = i;
    }
    int rc = 0;
    if (!exists && above && last >= 3) {        // the deepest folder there is (the volume's root at least)
        memcpy(part, path, last * sizeof *part);
        part[last] = 0;
        exists = true;
    }
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

// Whether `path` is a real folder (not a link) that a trusted account owns. With `dacl`, its access
// list is then replaced by that one (a folder made before this check existed may let users write).
static bool trusted_dir(const wchar_t *path, PACL dacl) {
    HANDLE d = CreateFileW(path, READ_CONTROL | (dacl ? WRITE_DAC : 0), FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (d == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION fi;
    PSID owner = NULL;
    PSECURITY_DESCRIPTOR sd = NULL;
    bool ok = GetFileInformationByHandle(d, &fi) && (fi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
              !(fi.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
              GetSecurityInfo(d, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &owner, NULL, NULL, NULL, &sd) == ERROR_SUCCESS && owner &&
              trusted_owner(owner);
    if (sd) LocalFree(sd);
    if (ok && dacl)
        ok = SetSecurityInfo(d, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, NULL, NULL, dacl, NULL) == ERROR_SUCCESS;
    CloseHandle(d);
    return ok;
}

// One folder that only trusted accounts may change: made here with `sd`, or there already as a real
// folder a trusted account owns (its access list then set to `sd`'s when `reset`). A folder a user
// made first - anywhere users may create folders, such as %ProgramData% or a volume's root - is
// refused: its owner could put other files there, or another folder in its place.
static bool secure_dir(MSIHANDLE h, const wchar_t *path, PSECURITY_DESCRIPTOR sd, bool reset) {
    SECURITY_ATTRIBUTES sa = { sizeof sa, sd, FALSE };
    if (path[0] == 0) return false;
    if (CreateDirectoryW(path, &sa)) return true;
    DWORD e = GetLastError();
    BOOL present = FALSE, dflt = FALSE;
    PACL dacl = NULL;
    bool ok = e == ERROR_ALREADY_EXISTS && GetSecurityDescriptorDacl(sd, &present, &dacl, &dflt) && present && dacl &&
              trusted_dir(path, reset ? dacl : NULL);
    if (!ok) log_line(h, L"rubrapack: %ls is not a folder of a trusted account, or could not be made (%lu)", path, (unsigned long)e);
    return ok;
}

__declspec(dllexport) UINT __stdcall RpGuardDirs(MSIHANDLE h) {
    wchar_t *list = get_property(h, L"RP_GUARD");
    if (list == NULL) return ERROR_INSTALL_FAILURE;
    // Per machine also the folder above a folder that is not there yet; per user those are the user's own.
    wchar_t *allusers = get_property(h, L"ALLUSERS");
    bool machine = allusers && allusers[0] == L'1';
    if (allusers) HeapFree(GetProcessHeap(), 0, allusers);
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
        if (check_dir(h, path, machine) == 0) continue;
        // The message in the chosen language, with [1] = the folder.
        wchar_t *lang = get_property(h, L"RPLANGUAGE");
        wchar_t name[64];
        wfmt(name, 64, L"RpGuardMsg_%ls", lang && lang[0] ? lang : L"en");
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
    wfmt(file, MAX_PATH, L"%ls.log", product && product[0] ? product : L"setup");
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
            wfmt(name, 64, L"RpLogMsg_%ls", lang && lang[0] ? lang : L"en");
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

// The cleanup folder of product `pc`: rubrapack\cleanup\<ProductCode> under the user's
// %LOCALAPPDATA% (per user) or under the computer's program data folder (per machine).
// Per machine the files there run as SYSTEM, so the folder is asked from Windows and only by the
// action that runs as SYSTEM: an action that runs as the user has the user's environment, where
// %ProgramData% is whatever the user set it to.
static bool cleanup_folder(const wchar_t *pc, bool machine, wchar_t out[MAX_PATH * 4]) {
    wchar_t base[MAX_PATH * 2];
    out[0] = 0;
    if (!is_guid(pc)) return false;
    if (machine) {
        if (SHGetFolderPathW(NULL, CSIDL_COMMON_APPDATA, NULL, SHGFP_TYPE_CURRENT, base) != S_OK) return false;
    } else {
        DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH * 2);
        if (n == 0 || n >= MAX_PATH * 2) return false;
    }
    size_t bl = wcslen(base);
    if (bl < 3 || base[1] != L':') return false;
    if (base[bl - 1] == L'\\') base[bl - 1] = 0;
    return wfmt(out, MAX_PATH * 4, L"%ls\\rubrapack\\cleanup\\%ls", base, pc) > 0;
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
    wfmt(file, MAX_PATH * 4, L"%ls\\pending.txt", folder);
    HANDLE f = CreateFileW(file, FILE_APPEND_DATA, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    int n = wfmt(line, MAX_PATH * 4 + 16, L"file\t%ls\r\n", path);
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
        wfmt(tag, 40, L"rp-%08lX%08lX", (unsigned long)GetCurrentProcessId(), (unsigned long)GetTickCount());
        // Where a file that cannot even be queued for the next restart (per user) is noted.
        // Per user only: a per-machine removal runs as SYSTEM, which can always queue.
        static wchar_t cfolder[MAX_PATH * 4];
        wchar_t *pc = get_property(h, L"ProductCode");
        if (wcscmp(root, L"volume") != 0 && pc && cleanup_folder(pc, false, cfolder)) w_rec(&apply, L"c", cfolder, L"");
        if (pc) HeapFree(GetProcessHeap(), 0, pc);
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
            wfmt(src, MAX_PATH * 4, L"%ls*", dir);
            WIN32_FIND_DATAW fd;
            HANDLE fh = FindFirstFileExW(src, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0);
            if (fh == INVALID_HANDLE_VALUE) continue;
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                if (!wild(f[1], fd.cFileName)) continue;
                wfmt(src, MAX_PATH * 4, L"%ls%ls", dir, fd.cFileName);
                if (wcscmp(root, L"volume") == 0 && GetVolumePathNameW(src, vol, MAX_PATH * 4)) {
                    wfmt(bdir, MAX_PATH * 4, L"%lsConfig.Msi\\%ls", vol, tag);
                } else {
                    DWORD tn = GetTempPathW(MAX_PATH * 4, vol);
                    if (tn == 0 || tn >= MAX_PATH * 4) continue;
                    wfmt(bdir, MAX_PATH * 4, L"%ls%ls", vol, tag);
                }
                wfmt(dst, MAX_PATH * 4, L"%ls\\%zu", bdir, n++);
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

// The folder above `path` when that folder is named Config.Msi (a per-machine backup folder is
// <volume>\Config.Msi\rp-<tag>; a per-user one is in the user's temporary folder), else false.
static bool config_msi_above(const wchar_t *path, wchar_t parent[MAX_PATH * 4]) {
    size_t n = wcslen(path);
    if (n >= MAX_PATH * 4) return false;
    memcpy(parent, path, (n + 1) * sizeof *parent);
    wchar_t *slash = wcsrchr(parent, L'\\');
    if (slash == NULL) return false;
    *slash = 0;
    slash = wcsrchr(parent, L'\\');
    return slash && _wcsicmp(slash + 1, L"Config.Msi") == 0;
}

// Whether the backup folder `dir` may be used. Per machine the moves run as SYSTEM, and users may
// create folders in a volume's root: a Config.Msi (or a backup folder in it) that a user made is
// the user's to change, so files would be moved back from - or deleted in - a place the user chose.
// Both must be real folders a trusted account owns. A per-user folder is the user's own.
static bool backup_ok(const wchar_t *dir) {
    static wchar_t parent[MAX_PATH * 4];
    if (!config_msi_above(dir, parent)) return true;
    return trusted_dir(parent, NULL) && trusted_dir(dir, NULL);
}

// The same for a file in a backup folder.
static bool backup_file_ok(const wchar_t *file) {
    static wchar_t dir[MAX_PATH * 4];
    size_t n = wcslen(file);
    if (n >= MAX_PATH * 4) return false;
    memcpy(dir, file, (n + 1) * sizeof *dir);
    wchar_t *slash = wcsrchr(dir, L'\\');
    if (slash == NULL) return false;
    *slash = 0;
    return backup_ok(dir);
}

// A backup folder (it holds removed files for a moment). Per machine: only SYSTEM and
// Administrators can open it, and it and Config.Msi above it are made here or owned by a trusted
// account already.
static void make_private_dir(MSIHANDLE h, const wchar_t *path) {
    static wchar_t parent[MAX_PATH * 4];
    bool ok;
    if (config_msi_above(path, parent)) {
        PSECURITY_DESCRIPTOR sd = NULL;
        ok = ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)", SDDL_REVISION_1, &sd, NULL) != 0;
        if (ok) {
            bool had = GetFileAttributesW(parent) != INVALID_FILE_ATTRIBUTES;
            ok = secure_dir(h, parent, sd, false);
            if (ok && !had) SetFileAttributesW(parent, FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);   // as Windows Installer makes it
            ok = ok && secure_dir(h, path, sd, true);
            LocalFree(sd);
        }
    } else {
        // The user's own temporary folder: the user's access list as it is (one naming only SYSTEM
        // and Administrators would shut a standard user out of the folder just made).
        ok = CreateDirectoryW(path, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
    }
    log_line(h, L"rubrapack: remove: backup folder %ls%ls", path, ok ? L"" : L": not usable");
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
                // The backup side of the move: f[2] going there, f[1] coming back.
                BOOL ok = backup_file_ok(to_backup ? f[2] : f[1]);
                if (!ok) SetLastError(ERROR_ACCESS_DENIED);
                ok = ok && MoveFileExW(f[1], f[2], MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH);
                DWORD e = ok ? 0 : GetLastError();
                log_line(h, L"rubrapack: remove: move %ls -> %ls: %lu", f[1], f[2], (unsigned long)e);
                if (!ok && to_backup) {          // held: it goes at the next restart instead
                    BOOL later = MoveFileExW(f[1], NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
                    log_line(h, L"rubrapack: remove: %ls at the next restart: %ls", f[1], later ? L"queued" : L"could not queue");
                    if (!later && note[0]) note_pending(h, note, f[1]);     // per user: the cleanup task's job
                }
            } else if (op == L'f') {
                if (!backup_file_ok(f[1])) continue;
                if (!DeleteFileW(f[1]) && GetLastError() != ERROR_FILE_NOT_FOUND) MoveFileExW(f[1], NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
            } else if (op == L'x') {
                if (!backup_ok(f[1])) continue;
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
    wfmt(num, 24, L"%lu", count);
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

// `path` is inside folder `dir`, ignoring case.
static bool inside(const wchar_t *path, const wchar_t *dir) {
    size_t n = wcslen(dir);
    return n && CompareStringOrdinal(path, (int)n, dir, (int)n, TRUE) == CSTR_EQUAL && path[n] == L'\\' && path[n + 1];
}

// <volume>\Config.Msi\<name>.rbf, one level down: one of the installer's backups.
static bool is_backup(const wchar_t *path) {
    size_t n = wcslen(path);
    return n > 17 && path[1] == L':' && CompareStringOrdinal(path + 2, 12, L"\\Config.Msi\\", 12, TRUE) == CSTR_EQUAL &&
           !wcschr(path + 14, L'\\') && CompareStringOrdinal(path + n - 4, 4, L".rbf", 4, TRUE) == CSTR_EQUAL;
}

// x46: the add-ons of the main product `uc` still installed, from one registry root and view.
static void addons_from(MSIHANDLE h, HKEY root, REGSAM view, const wchar_t *uc, wchar_t codes[][40], size_t *n, size_t cap) {
    wchar_t key[160];
    wfmt(key, 160, L"SOFTWARE\\rubrapack\\Addons\\%ls", uc);
    HKEY k;
    if (RegOpenKeyExW(root, key, 0, KEY_QUERY_VALUE | view, &k) != ERROR_SUCCESS) return;
    for (DWORD i = 0; *n < cap; ++i) {
        wchar_t name[64];
        DWORD nl = 64;
        LONG e = RegEnumValueW(k, i, name, &nl, NULL, NULL, NULL, NULL);
        if (e == ERROR_NO_MORE_ITEMS) break;
        if (e != ERROR_SUCCESS || nl != 38 || !is_guid(name)) continue;
        bool dup = false;
        for (size_t j = 0; j < *n; ++j) dup = dup || _wcsicmp(codes[j], name) == 0;
        if (dup || MsiQueryProductStateW(name) != INSTALLSTATE_DEFAULT) continue;
        wcscpy(codes[(*n)++], name);
        log_line(h, L"rubrapack: cleanup: add-on %ls will be removed after this removal", name);
    }
    RegCloseKey(k);
}

__declspec(dllexport) UINT __stdcall RpCleanupPrepare(MSIHANDLE h) {
    wchar_t *dirs = get_property(h, L"RP_CLEANUP_DIRS");
    wchar_t *scope = get_property(h, L"RP_CLEANUP_SCOPE");
    wchar_t *pc = get_property(h, L"ProductCode");
    writer_t w = { 0 };
    w_raw(&w, L"RPC1", 4);
    static wchar_t folder[MAX_PATH * 4];
    bool machine = scope && wcscmp(scope, L"machine") == 0;
    // Why this run may leave something. Windows Installer queues the installer's backups for the
    // next restart only after the commit actions (x40), so the commit cannot know - it registers the
    // task for every run that removes or replaces files, and the task's first run, two minutes
    // later, sees what there is (and removes itself when there is nothing).
    wchar_t *remove = get_property(h, L"REMOVE"), *older = get_property(h, L"RP_OLDER_FOUND"), *installed = get_property(h, L"Installed");
    const wchar_t *why = remove && remove[0] ? L"remove" : older && older[0] ? L"upgrade" : installed && installed[0] ? L"maintenance" : L"";
    // Per machine the folder is left empty here: the action that runs as SYSTEM finds it itself.
    if (dirs && pc && is_guid(pc) && (machine || cleanup_folder(pc, false, folder))) {
        w_field(&w, pc);
        w_field(&w, machine ? L"machine" : L"user");
        w_field(&w, machine ? L"" : folder);
        w_field(&w, why);
        // x46: the add-ons, on a real removal only (an upgrade removing this version keeps them).
        static wchar_t codes[64][40];
        size_t nadd = 0;
        wchar_t *want = get_property(h, L"RP_REMOVE_ADDONS"), *upg = get_property(h, L"UPGRADINGPRODUCTCODE");
        wchar_t *uc = get_property(h, L"UpgradeCode");
        bool addons = want && wcscmp(want, L"1") == 0 && remove && wcscmp(remove, L"ALL") == 0 && !(upg && upg[0]) && is_guid(uc);
        if (addons) {
            HKEY root = machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
            addons_from(h, root, KEY_WOW64_64KEY, uc, codes, &nadd, 64);
            addons_from(h, root, KEY_WOW64_32KEY, uc, codes, &nadd, 64);
        }
        w_field(&w, nadd ? uc : L"");
        wchar_t anum[24];
        wfmt(anum, 24, L"%zu", nadd);
        w_field(&w, anum);
        for (size_t i = 0; i < nadd; ++i) w_field(&w, codes[i]);
        if (want) HeapFree(GetProcessHeap(), 0, want);
        if (upg) HeapFree(GetProcessHeap(), 0, upg);
        if (uc) HeapFree(GetProcessHeap(), 0, uc);
        static wchar_t paths[64][MAX_PATH * 4];
        size_t np = 0;
        wchar_t *ctx = NULL;
        for (wchar_t *d = wcstok(dirs, L";", &ctx); d && np < 64; d = wcstok(NULL, L";", &ctx)) {
            DWORD n = MAX_PATH * 4;
            if (MsiGetTargetPathW(h, d, paths[np], &n) != ERROR_SUCCESS) continue;
            size_t len = wcslen(paths[np]);
            if (len > 3 && paths[np][len - 1] == L'\\') paths[np][len - 1] = 0;
            ++np;
        }
        wchar_t num[24];
        wfmt(num, 24, L"%zu", np);
        w_field(&w, num);
        for (size_t i = 0; i < np; ++i) w_field(&w, paths[i]);
        // The deletions already queued under those folders (and, per machine, the installer's
        // backups) before this installation's script runs: anything queued there later is this
        // installation's (a held file's deletion during the script, the backups at its end - x40).
        // Paths, not positions: other tasks take entries out of the list (x42).
        HKEY k;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, PFRO_KEY, 0, KEY_QUERY_VALUE, &k) == ERROR_SUCCESS) {
            DWORD size = 0;
            wchar_t *buf = pfro_read(k, &size);
            RegCloseKey(k);
            for (wchar_t *src = buf; src && *src;) {
                wchar_t *dst = src + wcslen(src) + 1;
                const wchar_t *p = pfro_path(src);
                bool hit = *dst == 0 && machine && is_backup(p);
                for (size_t i = 0; *dst == 0 && !hit && i < np; ++i) hit = inside(p, paths[i]);
                if (hit) w_field(&w, p);
                src = *dst ? dst + wcslen(dst) + 1 : dst + 1;
            }
            if (buf) HeapFree(GetProcessHeap(), 0, buf);
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
    wfmt(iso, 24, L"%04u-%02u-%02uT%02u:%02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    wfmt(ymd, 12, L"%04u%02u%02u", st.wYear, st.wMonth, st.wDay);
}

static void when(int days, wchar_t iso[24], wchar_t ymd[12]) { when_minutes((long long)days * 1440, iso, ymd); }

__declspec(dllexport) UINT __stdcall RpCleanupRegister(MSIHANDLE h) {
    wchar_t *data = get_property(h, L"CustomActionData");
    if (data == NULL) return ERROR_SUCCESS;
    reader_t r = { data, data + wcslen(data) };
    static wchar_t pc[64], scope[16], folder[MAX_PATH * 4], why[16], dirs[64][MAX_PATH * 4], parent[64], addons[64][40];
    size_t ndirs = 0, naddons = 0;
    if (wcsncmp(data, L"RPC1", 4) != 0) goto done;
    r.p += 4;
    static wchar_t num[24], before[MAX_PATH * 4];
    if (!r_field(&r, pc, 64) || !r_field(&r, scope, 16) || !r_field(&r, folder, MAX_PATH * 4) || !r_field(&r, why, 16) ||
        !r_field(&r, parent, 64) || !r_field(&r, num, 24))
        goto done;
    for (unsigned long want = wcstoul(num, NULL, 10); naddons < want && naddons < 64 && r_field(&r, addons[naddons], 40);) ++naddons;
    if (!r_field(&r, num, 24)) goto done;
    for (unsigned long want = wcstoul(num, NULL, 10); ndirs < want && ndirs < 64 && r_field(&r, dirs[ndirs], MAX_PATH * 4);) ++ndirs;
    const wchar_t *befores = r.p;         // the rest: the deletions queued before the script
    bool machine = wcscmp(scope, L"machine") == 0;
    // The codes go into paths, the task's name and command lines; per machine the folder is not
    // taken from the data (which an action running as the user wrote) but asked from Windows.
    if (!is_guid(pc) || (naddons && !is_guid(parent))) goto done;
    for (size_t i = 0; i < naddons; ++i) {
        if (!is_guid(addons[i])) goto done;
    }
    if (machine ? !cleanup_folder(pc, true, folder) : folder[0] == 0) {
        log_line(h, L"rubrapack: cleanup: no folder for the cleanup task; nothing registered");
        goto done;
    }
    writer_t list = { 0 };
    size_t found = 0;
    // Files a per-user package could not queue (RpRemoveApply wrote them down). Never per machine.
    static wchar_t pend[MAX_PATH * 4];
    wfmt(pend, MAX_PATH * 4, L"%ls\\pending.txt", folder);
    writer_t files = { 0 };
    HANDLE pf = machine ? INVALID_HANDLE_VALUE : CreateFileW(pend, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
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
    log_line(h, L"rubrapack: cleanup: run kind '%ls', %zu file(s) noted, %zu add-on(s)", why, found, naddons);
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
    if (machine || sid == NULL) wfmt(name, 160, L"rubrapack cleanup %ls", pc);
    else wfmt(name, 160, L"rubrapack cleanup %ls %ls", pc, sid);
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
    for (reader_t b = { befores, r.end }; r_field(&b, before, MAX_PATH * 4);) {
        w_text(&list, L"before\t"); w_text(&list, before); w_text(&list, L"\r\n");
    }
    for (size_t i = 0; i < ndirs; ++i) {
        w_text(&list, L"root\t"); w_text(&list, dirs[i]); w_text(&list, L"\r\n");
    }
    if (naddons) { w_text(&list, L"parent\t"); w_text(&list, parent); w_text(&list, L"\r\n"); }
    for (size_t i = 0; i < naddons; ++i) {
        w_text(&list, L"addon\t"); w_text(&list, addons[i]); w_text(&list, L"\r\n");
    }
    if (files.buf) w_text(&list, files.buf);
    // The folder: per machine only SYSTEM and Administrators may write (the task runs as SYSTEM).
    // %ProgramData% lets users create folders, so each of the three levels - rubrapack, cleanup,
    // <ProductCode> - is made here with that access list or is already a real folder a trusted
    // account owns; the owner of a level a user made could put another folder (and another
    // program) in place of the one below it.
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, FALSE };
    if (machine) {
        static wchar_t base[MAX_PATH * 4], mid[MAX_PATH * 4];
        wcscpy(mid, folder);
        wchar_t *slash = wcsrchr(mid, L'\\');
        if (slash) *slash = 0;
        wcscpy(base, mid);
        slash = wcsrchr(base, L'\\');
        if (slash) *slash = 0;
        bool safe = ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;GRGX;;;BU)", SDDL_REVISION_1,
                                                                         &sa.lpSecurityDescriptor, NULL) != 0;
        safe = safe && secure_dir(h, base, sa.lpSecurityDescriptor, true) && secure_dir(h, mid, sa.lpSecurityDescriptor, true) &&
               secure_dir(h, folder, sa.lpSecurityDescriptor, true);
        if (!safe) {
            log_line(h, L"rubrapack: cleanup: %ls is not safe to use; nothing registered", folder);
            if (files.buf) HeapFree(GetProcessHeap(), 0, files.buf);
            if (list.buf) HeapFree(GetProcessHeap(), 0, list.buf);
            if (sa.lpSecurityDescriptor) LocalFree(sa.lpSecurityDescriptor);
            goto done;
        }
    } else {
        make_dirs(folder);
    }
    wfmt(exe, MAX_PATH * 4, L"%ls\\rubrapack_clean.exe", folder);
    wfmt(path, MAX_PATH * 4, L"%ls\\list.txt", folder);
    // New files, with the folder's access list: never one left there with another owner.
    DeleteFileW(exe);
    DeleteFileW(path);
    bool ok = write_file(exe, rp_clean_part, rp_clean_part_len, sa.lpSecurityDescriptor ? &sa : NULL) &&
              write_file(path, list.buf, list.len * sizeof(wchar_t), sa.lpSecurityDescriptor ? &sa : NULL);
    writer_t x = { 0 };
    w_raw(&x, L"\xFEFF", 1);
    w_text(&x, L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n");
    w_text(&x, L"<RegistrationInfo><Description>rubrapack: deletes what the installation of ");
    w_xml(&x, pc);
    w_text(&x, L" had to leave until the next restart, as soon as nothing holds it.</Description>");
    // Per user the task runs without elevation (below), so the user is given the task itself: an
    // elevated installation registered it, and it must still be able to delete itself at the end.
    if (sid) {
        w_text(&x, L"<SecurityDescriptor>D:(A;;FA;;;SY)(A;;FA;;;BA)(A;;FA;;;");
        w_text(&x, sid);
        w_text(&x, L")</SecurityDescriptor>");
    }
    w_text(&x, L"</RegistrationInfo>\r\n<Triggers>\r\n<LogonTrigger><StartBoundary>");
    w_text(&x, iso0); w_text(&x, L"</StartBoundary><EndBoundary>"); w_text(&x, iso1); w_text(&x, L"</EndBoundary><Enabled>true</Enabled>");
    if (sid) { w_text(&x, L"<UserId>"); w_text(&x, sid); w_text(&x, L"</UserId>"); }
    w_text(&x, L"</LogonTrigger>\r\n<TimeTrigger><Repetition><Interval>PT15M</Interval><StopAtDurationEnd>false</StopAtDurationEnd></Repetition><StartBoundary>");
    w_text(&x, isofirst); w_text(&x, L"</StartBoundary><EndBoundary>"); w_text(&x, iso1); w_text(&x, L"</EndBoundary><Enabled>true</Enabled></TimeTrigger>\r\n</Triggers>\r\n");
    w_text(&x, L"<Principals><Principal id=\"Author\"><UserId>");
    w_text(&x, machine ? L"S-1-5-18" : sid ? sid : L"");
    // Never elevated for a user: the program is in a folder the user's own programs can write to,
    // and a task that ran it with an administrator's full rights would hand them those rights.
    w_text(&x, machine ? L"</UserId><RunLevel>HighestAvailable</RunLevel>" : L"</UserId><LogonType>InteractiveToken</LogonType><RunLevel>LeastPrivilege</RunLevel>");
    w_text(&x, L"</Principal></Principals>\r\n<Settings><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy><DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>"
               L"<StopIfGoingOnBatteries>false</StopIfGoingOnBatteries><StartWhenAvailable>true</StartWhenAvailable><ExecutionTimeLimit>PT10M</ExecutionTimeLimit>"
               L"<DeleteExpiredTaskAfter>PT0S</DeleteExpiredTaskAfter></Settings>\r\n<Actions Context=\"Author\"><Exec><Command>");
    w_xml(&x, exe);
    w_text(&x, L"</Command><Arguments>\"");
    w_xml(&x, folder);
    w_text(&x, L"\"</Arguments></Exec></Actions>\r\n</Task>\r\n");
    if (sid) LocalFree(sid);
    wfmt(path, MAX_PATH * 4, L"%ls\\task.xml", folder);
    ok = ok && !x.bad && !list.bad && write_file(path, x.buf, x.len * sizeof(wchar_t), NULL);
    GetSystemDirectoryW(sys, MAX_PATH);
    if (ok) {
        wfmt(cmd, MAX_PATH * 8, L"\"%ls\\schtasks.exe\" /create /tn \"%ls\" /xml \"%ls\" /f", sys, name, path);
        run_wait(h, cmd);     // first run two minutes from now: the engine's own deletions are queued by then
        // With add-ons to remove it also runs right away: rubrapack_clean.exe waits for this
        // installation to end, so the add-ons go seconds after the main product (x46).
        if (naddons) {
            wfmt(cmd, MAX_PATH * 8, L"\"%ls\\schtasks.exe\" /run /tn \"%ls\"", sys, name);
            run_wait(h, cmd);
        }
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

// ---- the Windows 11 menu's identity package (plan 2026-10-09; rp_appx.h) -----------------------
//
// RpMenuRegister / RpMenuRemove, deferred (SYSTEM per machine, the user per user), each also the
// other's rollback. CustomActionData: "<package file>|RPM1|m or u|u or s|<full name>|<family name>",
// then the classic verbs' keys.
// The package's external location is the folder the file is in (the program's). Never a failure of
// the installation: without the package the classic menu still has the items.

static bool package_name_ok(const wchar_t *s) {
    if (s[0] == 0 || wcslen(s) > 200) return false;
    for (; *s; ++s) {
        if (!((*s >= L'A' && *s <= L'Z') || (*s >= L'a' && *s <= L'z') || (*s >= L'0' && *s <= L'9') || *s == L'.' || *s == L'-' || *s == L'_')) return false;
    }
    return true;
}

// Whether a registry key has neither values nor sub keys.
static bool key_empty(HKEY root, const wchar_t *path) {
    HKEY k;
    if (RegOpenKeyExW(root, path, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS) return false;
    DWORD subkeys = 1, values = 1;
    LONG e = RegQueryInfoKeyW(k, NULL, NULL, NULL, &subkeys, NULL, NULL, &values, NULL, NULL, NULL, NULL);
    RegCloseKey(k);
    return e == ERROR_SUCCESS && subkeys == 0 && values == 0;
}

// The classic verb at Software\Classes\<key>: switched off (the package's item shows there too)
// or, at removal, the switch taken away again - and the key with it once the installer has removed
// its own values (this action runs after that), with the "shell" key above when it is left empty.
static void legacy_verb(MSIHANDLE h, bool machine, const wchar_t *key, bool off) {
    static wchar_t path[MAX_PATH * 2];
    if (wfmt(path, MAX_PATH * 2, L"Software\\Classes\\%ls", key) < 0 || wcsstr(key, L"\\shell\\") == NULL) return;
    HKEY root = machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER, k;
    if (off) {
        if (RegOpenKeyExW(root, path, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;       // only a verb that is there
        RegSetValueExW(k, L"LegacyDisable", 0, REG_SZ, (const BYTE *)L"", sizeof(wchar_t));
        RegCloseKey(k);
        return;
    }
    if (RegOpenKeyExW(root, path, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    RegDeleteValueW(k, L"LegacyDisable");
    RegCloseKey(k);
    for (int up = 0; up < 2 && key_empty(root, path); ++up) {
        if (RegDeleteKeyW(root, path) != ERROR_SUCCESS) break;
        wchar_t *slash = wcsrchr(path, L'\\');
        if (slash == NULL) break;
        *slash = 0;
        if (up == 0 && _wcsicmp(slash - 6 > path ? slash - 6 : path, L"\\shell") != 0) break;   // only the "shell" key above
    }
    (void)h;
}

static void menu_job(MSIHANDLE h, bool remove) {
    wchar_t *data = get_property(h, L"CustomActionData");
    if (data == NULL) return;
    enum { MAX_VERBS = 600 };
    static wchar_t *f[6 + MAX_VERBS];
    size_t n = 0;
    for (wchar_t *p = data; n < 6 + MAX_VERBS;) {
        f[n++] = p;
        wchar_t *bar = wcschr(p, L'|');
        if (bar == NULL) break;
        *bar = 0;
        p = bar + 1;
    }
    static wchar_t dir[MAX_PATH * 4];
    bool ok = n >= 6 && wcscmp(f[1], L"RPM1") == 0 && package_name_ok(f[4]) && package_name_ok(f[5]) && wcslen(f[0]) < MAX_PATH * 4;
    if (ok) {
        wcscpy(dir, f[0]);
        wchar_t *slash = wcsrchr(dir, L'\\');
        ok = slash != NULL && slash > dir + 2;
        if (ok) *slash = 0;
    }
    if (!ok) {
        log_line(h, L"rubrapack: menu: no usable data for the identity package; nothing done");
    } else {
        appx_job_t j;
        memset(&j, 0, sizeof j);
        j.machine = f[2][0] == L'm';
        j.allow_unsigned = f[3][0] == L'u';
        j.remove = remove;
        j.package = f[0];
        j.external = dir;
        j.full_name = f[4];
        j.family = f[5];
        HRESULT hr = appx_run(&j);
        if (SUCCEEDED(hr)) {
            log_line(h, L"rubrapack: menu: identity package %ls %ls (%ls)", f[5], remove ? L"removed" : L"registered", j.machine ? L"for all users" : L"for this user");
        } else if (remove) {
            log_line(h, L"rubrapack: menu: identity package %ls not removed: 0x%08lX at %ls (it may not have been registered)", f[4], (unsigned long)hr, j.step);
        } else {
            log_line(h, L"rubrapack: menu: identity package %ls not registered: 0x%08lX at %ls; the items are in the classic menu only", f[5],
                     (unsigned long)hr, j.step);
        }
        // Registered: the classic verbs off. Removed: the switch and the emptied keys taken away.
        // A registration that failed changes nothing (a repair must not switch them on again).
        if (remove || SUCCEEDED(hr)) {
            for (size_t i = 6; i < n; ++i) legacy_verb(h, j.machine, f[i], !remove);
        }
    }
    HeapFree(GetProcessHeap(), 0, data);
}

__declspec(dllexport) UINT __stdcall RpMenuRegister(MSIHANDLE h) {
    menu_job(h, false);
    return ERROR_SUCCESS;
}

__declspec(dllexport) UINT __stdcall RpMenuRemove(MSIHANDLE h) {
    menu_job(h, true);
    return ERROR_SUCCESS;
}

// ---- preflight (DECISIONS 2026-10-06) -------------------------------------------------------------
//
// RpPreflight, immediate, before InstallValidate in the execute sequence (every UI level, also a
// removal started from Installed apps). It only reads - and, when asked, closes programs - so a
// refusal leaves the computer as it was.
//   1. The programs using the product's files (its own programs and those that have one of its
//      DLLs loaded), from Restart Manager: their number and names are shown and the user is asked
//      whether to close them all - Yes closes them (a close request to their windows, then, after
//      a few seconds, ended), No goes on without closing (they keep the old files until reopened),
//      Cancel stops. Without a window to ask in (/qn) the installation fails, unless RPCLOSE says
//      "yes" (close them all) or "no" (go on). RP_PREFLIGHT_CLOSE is the package's own default:
//      "ask", "always" or "never".
//   2. For an installation or an upgrade (never a removal): every package folder that exists is a
//      folder SYSTEM may delete in, and the installed older versions still have their cached
//      packages - otherwise the message says what is wrong and to remove the product first.
// Texts: RpPre<Name>_<language> properties (RPLANGUAGE, else Korean for a Korean user, else en).

#include <restartmanager.h>

static wchar_t *pre_text(MSIHANDLE h, const wchar_t *id) {
    wchar_t name[80];
    wchar_t *lang = get_property(h, L"RPLANGUAGE");
    wchar_t *text = NULL;
    if (lang && lang[0]) {
        wfmt(name, 80, L"RpPre%ls_%ls", id, lang);
        text = get_property(h, name);
    } else {
        wchar_t *ul = get_property(h, L"UserLanguageID");
        if (ul && wcscmp(ul, L"1042") == 0) {
            wfmt(name, 80, L"RpPre%ls_ko", id);
            text = get_property(h, name);
        }
        if (ul) HeapFree(GetProcessHeap(), 0, ul);
    }
    if (text == NULL || text[0] == 0) {
        if (text) HeapFree(GetProcessHeap(), 0, text);
        wfmt(name, 80, L"RpPre%ls_en", id);
        text = get_property(h, name);
    }
    if (lang) HeapFree(GetProcessHeap(), 0, lang);
    return text;
}

// Shows (or, without UI, logs) a message made from the text `id` with fields 1..3; returns the button.
static int pre_message(MSIHANDLE h, const wchar_t *id, UINT type, const wchar_t *f1, const wchar_t *f2, const wchar_t *f3) {
    wchar_t *text = pre_text(h, id);
    MSIHANDLE rec = MsiCreateRecord(3);
    MsiRecordSetStringW(rec, 0, text && text[0] ? text : L"[1] [2] [3]");
    MsiRecordSetStringW(rec, 1, f1 ? f1 : L"");
    MsiRecordSetStringW(rec, 2, f2 ? f2 : L"");
    MsiRecordSetStringW(rec, 3, f3 ? f3 : L"");
    int r = MsiProcessMessage(h, (INSTALLMESSAGE)type, rec);
    MsiCloseHandle(rec);
    if (text) HeapFree(GetProcessHeap(), 0, text);
    return r;
}

static BOOL CALLBACK close_window(HWND w, LPARAM pid) {
    DWORD owner = 0;
    GetWindowThreadProcessId(w, &owner);
    if (owner == (DWORD)pid && GetWindow(w, GW_OWNER) == NULL) PostMessageW(w, WM_CLOSE, 0, 0);
    return TRUE;
}

// The image file name of a process ("" when it cannot be read).
static void image_name(DWORD pid, wchar_t out[MAX_PATH]) {
    out[0] = 0;
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (p == NULL) return;
    wchar_t path[MAX_PATH * 2];
    DWORD n = MAX_PATH * 2;
    if (QueryFullProcessImageNameW(p, 0, path, &n)) {
        const wchar_t *slash = wcsrchr(path, L'\\');
        wcsncpy(out, slash ? slash + 1 : path, MAX_PATH - 1);
        out[MAX_PATH - 1] = 0;
    }
    CloseHandle(p);
}

// Whether SYSTEM may delete inside folder `dir`, from its access list (true when it cannot be read:
// this action runs as the user and must not refuse on what it cannot see).
static bool system_may_delete(const wchar_t *dir) {
    PACL dacl = NULL;
    PSECURITY_DESCRIPTOR sd = NULL;
    if (GetNamedSecurityInfoW(dir, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, &dacl, NULL, &sd) != ERROR_SUCCESS) return true;
    bool ok = true;
    PSID sys = NULL;
    if (dacl && ConvertStringSidToSidW(L"S-1-5-18", &sys)) {
        TRUSTEEW t;
        memset(&t, 0, sizeof t);
        t.TrusteeForm = TRUSTEE_IS_SID;
        t.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
        t.ptstrName = (LPWSTR)sys;
        ACCESS_MASK rights = 0;
        if (GetEffectiveRightsFromAclW(dacl, &t, &rights) == ERROR_SUCCESS) ok = (rights & FILE_DELETE_CHILD) && (rights & FILE_ADD_FILE);
        LocalFree(sys);
    }
    if (sd) LocalFree(sd);
    return ok;
}

__declspec(dllexport) UINT __stdcall RpPreflight(MSIHANDLE h) {
    wchar_t *product = get_property(h, L"ProductName"), *uilevel = get_property(h, L"UILevel");
    wchar_t *answer = get_property(h, L"RPCLOSE"), *dflt = get_property(h, L"RP_PREFLIGHT_CLOSE");
    wchar_t *remove = get_property(h, L"REMOVE"), *older = get_property(h, L"RP_OLDER_FOUND");
    wchar_t *dirs = get_property(h, L"RP_PREFLIGHT_DIRS"), *code = get_property(h, L"ProductCode");
    UINT rc = ERROR_SUCCESS;
    bool silent = uilevel == NULL || wcstol(uilevel, NULL, 10) <= 2;
    bool removal = remove && _wcsicmp(remove, L"ALL") == 0;
    // The old version's removal inside an upgrade: the upgrading package has asked already (x51).
    wchar_t *upgrading = get_property(h, L"UPGRADINGPRODUCTCODE");
    bool nested = upgrading && upgrading[0];
    if (upgrading) HeapFree(GetProcessHeap(), 0, upgrading);
    if (nested) {
        log_line(h, L"rubrapack: preflight: skipped (removed by an upgrade, which has done it)");
        wchar_t *early[] = { product, uilevel, answer, dflt, remove, older, dirs, code };
        for (size_t i = 0; i < sizeof early / sizeof early[0]; ++i) {
            if (early[i]) HeapFree(GetProcessHeap(), 0, early[i]);
        }
        return ERROR_SUCCESS;
    }

    // 1. Who uses the product's files.
    enum { MAX_FILES = 2048, MAX_PROCS = 256 };
    static wchar_t *files[MAX_FILES];
    static wchar_t menu_dll[MAX_PATH * 4];      // rubrapack's menu part among the product's files
    menu_dll[0] = 0;
    UINT nfiles = 0;
    MSIHANDLE db = MsiGetActiveDatabase(h), view = 0, rec = 0;
    if (db && MsiDatabaseOpenViewW(db, L"SELECT `File`.`FileName`, `Component`.`Directory_` FROM `File`, `Component` "
                                       L"WHERE `File`.`Component_` = `Component`.`Component`", &view) == ERROR_SUCCESS &&
        MsiViewExecute(view, 0) == ERROR_SUCCESS) {
        static wchar_t fname[1024], dir[80], path[MAX_PATH * 4];
        while (nfiles < MAX_FILES && MsiViewFetch(view, &rec) == ERROR_SUCCESS) {
            DWORD fn = 1024, dn = 80, pn = MAX_PATH * 4;
            if (MsiRecordGetStringW(rec, 1, fname, &fn) == ERROR_SUCCESS && MsiRecordGetStringW(rec, 2, dir, &dn) == ERROR_SUCCESS &&
                MsiGetTargetPathW(h, dir, path, &pn) == ERROR_SUCCESS) {
                wchar_t *bar = wcschr(fname, L'|');
                const wchar_t *name = bar ? bar + 1 : fname;
                size_t len = wcslen(path);
                if (len + wcslen(name) < MAX_PATH * 4) {
                    wcscpy(path + len, name);
                    if (_wcsicmp(name, L"rubrapack_menu.dll") == 0 && wcslen(path) < MAX_PATH * 4) wcscpy(menu_dll, path);
                    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) {
                        size_t n = wcslen(path) + 1;
                        wchar_t *copy = HeapAlloc(GetProcessHeap(), 0, n * sizeof *copy);
                        if (copy) {
                            memcpy(copy, path, n * sizeof *copy);
                            files[nfiles++] = copy;
                        }
                    }
                }
            }
            MsiCloseHandle(rec);
        }
    }
    if (view) MsiCloseHandle(view);
    if (db) MsiCloseHandle(db);

    static RM_PROCESS_INFO procs[MAX_PROCS];
    static DWORD pids[MAX_PROCS];
    UINT nprocs = 0, nlisted = 0;
    writer_t names = { 0 };
    // The menu part's own host first: after someone used a [menu.*] item, a dllhost.exe keeps
    // rubrapack_menu.dll loaded for a while. It serves this package's menu classes only and holds
    // nothing of the user's, so it is ended without a question - it is not "a program using the
    // product's files" anyone could be asked to close (Explorer starts a new one when needed).
    if (menu_dll[0] && GetFileAttributesW(menu_dll) != INVALID_FILE_ATTRIBUTES) {
        DWORD session = 0;
        wchar_t key[CCH_RM_SESSION_KEY + 1] = L"";
        if (RmStartSession(&session, 0, key) == ERROR_SUCCESS) {
            LPCWSTR one = menu_dll;
            if (RmRegisterResources(session, 1, &one, 0, NULL, 0, NULL) == ERROR_SUCCESS) {
                UINT needed = 0, have = MAX_PROCS;
                DWORD reasons = 0, e = RmGetList(session, &needed, &have, procs, &reasons);
                for (UINT i = 0; (e == ERROR_SUCCESS || e == ERROR_MORE_DATA) && i < have; ++i) {
                    wchar_t image[MAX_PATH];
                    image_name(procs[i].Process.dwProcessId, image);
                    if (_wcsicmp(image, L"dllhost.exe") != 0) continue;
                    HANDLE p = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, procs[i].Process.dwProcessId);
                    BOOL ended = p && TerminateProcess(p, 0);
                    if (ended) WaitForSingleObject(p, 3000);
                    if (p) CloseHandle(p);
                    log_line(h, L"rubrapack: preflight: the menu part's host (dllhost.exe, process %lu) %ls",
                             (unsigned long)procs[i].Process.dwProcessId, ended ? L"ended" : L"could not be ended");
                }
            }
            RmEndSession(session);
        }
    }
    if (nfiles) {
        DWORD session = 0;
        wchar_t key[CCH_RM_SESSION_KEY + 1] = L"";
        if (RmStartSession(&session, 0, key) == ERROR_SUCCESS) {
            if (RmRegisterResources(session, nfiles, (LPCWSTR *)files, 0, NULL, 0, NULL) == ERROR_SUCCESS) {
                UINT needed = 0, have = MAX_PROCS;
                DWORD reasons = 0;
                DWORD e = RmGetList(session, &needed, &have, procs, &reasons);
                if (e == ERROR_SUCCESS || e == ERROR_MORE_DATA) nprocs = have;
                else log_line(h, L"rubrapack: preflight: Restart Manager could not list the programs (%lu)", (unsigned long)e);
            }
            RmEndSession(session);
        }
    }
    for (UINT i = 0; i < nprocs; ++i) {
        DWORD pid = procs[i].Process.dwProcessId;
        wchar_t image[MAX_PATH];
        image_name(pid, image);
        // Not the installer itself, and nothing Windows cannot do without.
        if (pid == GetCurrentProcessId() || _wcsicmp(image, L"msiexec.exe") == 0 || procs[i].ApplicationType == RmCritical) continue;
        log_line(h, L"rubrapack: preflight: in use by %ls (%ls, process %lu, kind %d)", procs[i].strAppName, image, (unsigned long)pid, (int)procs[i].ApplicationType);
        if (nlisted < 15) {
            wchar_t line[400];
            wfmt(line, 400, L"  - %ls (%ls)\r\n", procs[i].strAppName[0] ? procs[i].strAppName : image, image[0] ? image : L"?");
            w_raw(&names, line, wcslen(line));
        }
        pids[nlisted++] = pid;
        if (nlisted == MAX_PROCS) break;
    }
    if (nlisted > 15) {
        wchar_t more[64];
        wfmt(more, 64, L"  ... +%u\r\n", nlisted - 15);
        w_raw(&names, more, wcslen(more));
    }
    if (nlisted) {
        wchar_t count[16];
        wfmt(count, 16, L"%u", nlisted);
        // yes / no from RPCLOSE, else the package's default, else the question.
        int choice = 0;         // IDYES, IDNO, IDCANCEL
        if (answer && (_wcsicmp(answer, L"yes") == 0 || wcscmp(answer, L"1") == 0)) choice = IDYES;
        else if (answer && (_wcsicmp(answer, L"no") == 0 || wcscmp(answer, L"0") == 0)) choice = IDNO;
        else if (dflt && wcscmp(dflt, L"always") == 0) choice = IDYES;
        else if (dflt && wcscmp(dflt, L"never") == 0) choice = IDNO;
        else if (silent) {
            log_line(h, L"rubrapack: preflight: stopped: %u program(s) use the product's files and there is no window to ask in (RPCLOSE=yes or no)", nlisted);
            pre_message(h, L"Silent", INSTALLMESSAGE_ERROR | MB_OK | MB_ICONWARNING, count, names.buf ? names.buf : L"", product);
            rc = ERROR_INSTALL_FAILURE;
        } else {
            choice = pre_message(h, L"Ask", INSTALLMESSAGE_USER | MB_YESNOCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2, count, names.buf ? names.buf : L"", product);
            if (choice != IDYES && choice != IDNO) rc = ERROR_INSTALL_USEREXIT;
        }
        if (rc == ERROR_SUCCESS && choice == IDYES) {
            // A close request to every window, a few seconds to save and go, then the end.
            HANDLE open[MAX_PROCS];
            for (UINT i = 0; i < nlisted; ++i) {
                open[i] = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pids[i]);
                EnumWindows(close_window, (LPARAM)pids[i]);
            }
            DWORD until = GetTickCount() + 6000;
            for (UINT i = 0; i < nlisted; ++i) {
                if (open[i] == NULL) {
                    log_line(h, L"rubrapack: preflight: process %lu could not be opened (%lu); it keeps the old files until it is reopened",
                             (unsigned long)pids[i], (unsigned long)GetLastError());
                    continue;
                }
                DWORD now = GetTickCount();
                if (WaitForSingleObject(open[i], now < until ? until - now : 0) != WAIT_OBJECT_0) {
                    BOOL ended = TerminateProcess(open[i], 1);
                    log_line(h, L"rubrapack: preflight: process %lu ended: %ls", (unsigned long)pids[i], ended ? L"yes" : L"no");
                    if (ended) WaitForSingleObject(open[i], 3000);
                } else {
                    log_line(h, L"rubrapack: preflight: process %lu closed", (unsigned long)pids[i]);
                }
                CloseHandle(open[i]);
            }
        } else if (rc == ERROR_SUCCESS) {
            log_line(h, L"rubrapack: preflight: going on without closing %u program(s)", nlisted);
        }
    }

    // 2. Folders and the older versions' cached packages - not for a removal, which must stay possible.
    if (rc == ERROR_SUCCESS && !removal) {
        wchar_t path[MAX_PATH * 4], cmd[160];
        // The first older version's code (the list is "{..};{..}").
        wfmt(cmd, 160, L"msiexec /x %.38ls", older && older[0] ? older : code ? code : L"");
        wchar_t *ctx = NULL;
        for (wchar_t *d = dirs ? wcstok(dirs, L";", &ctx) : NULL; d && rc == ERROR_SUCCESS; d = wcstok(NULL, L";", &ctx)) {
            DWORD n = MAX_PATH * 4;
            if (MsiGetTargetPathW(h, d, path, &n) != ERROR_SUCCESS) continue;
            size_t len = wcslen(path);
            if (len > 3 && path[len - 1] == L'\\') path[len - 1] = 0;
            DWORD a = GetFileAttributesW(path);
            if (a == INVALID_FILE_ATTRIBUTES) continue;
            if (!(a & FILE_ATTRIBUTE_DIRECTORY) || !system_may_delete(path)) {
                log_line(h, L"rubrapack: preflight: refused: %ls is not a folder SYSTEM may delete in", path);
                pre_message(h, L"Folder", INSTALLMESSAGE_ERROR | MB_OK | MB_ICONWARNING, path, cmd, product);
                rc = ERROR_INSTALL_FAILURE;
            }
        }
        ctx = NULL;
        for (wchar_t *pc = older ? wcstok(older, L";", &ctx) : NULL; pc && rc == ERROR_SUCCESS; pc = wcstok(NULL, L";", &ctx)) {
            wchar_t pkg[MAX_PATH * 2];
            DWORD n = MAX_PATH * 2;
            if (MsiGetProductInfoW(pc, L"LocalPackage", pkg, &n) != ERROR_SUCCESS) continue;
            if (pkg[0] && GetFileAttributesW(pkg) == INVALID_FILE_ATTRIBUTES) {
                wfmt(cmd, 160, L"msiexec /x %ls", pc);
                log_line(h, L"rubrapack: preflight: refused: the cached package of %ls is missing (%ls)", pc, pkg);
                pre_message(h, L"Cache", INSTALLMESSAGE_ERROR | MB_OK | MB_ICONWARNING, pc, cmd, product);
                rc = ERROR_INSTALL_FAILURE;
            }
        }
    }
    for (UINT i = 0; i < nfiles; ++i) HeapFree(GetProcessHeap(), 0, files[i]);
    wchar_t *all[] = { product, uilevel, answer, dflt, remove, older, dirs, code };
    for (size_t i = 0; i < sizeof all / sizeof all[0]; ++i) {
        if (all[i]) HeapFree(GetProcessHeap(), 0, all[i]);
    }
    if (names.buf) HeapFree(GetProcessHeap(), 0, names.buf);
    return rc;
}
