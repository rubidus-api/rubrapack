// src/ca/rubrapack_ca.c - rubrapack's helper custom-action DLL (RFC-0001 9.6, 9.6.1; RFC-0004).
//
// Built for x64, x86 and Arm64 by `nob parts` and stored in resources/bin/; `build` puts the one
// for the package's architecture into the Binary table. It depends on the OS only (msi.dll,
// advapi32, kernel32, ntdll, user32, comdlg32 and the Universal CRT).
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
// Wiping at removal (DECISIONS 2026-10-11): RpPurgePrepare, RpPurge - described where they are.
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
        // Per user, a real removal: the files of the components that go. One a program still holds
        // stays behind, and without administrator rights Windows Installer cannot queue it for the
        // next restart - the commit action notes those still there for the cleanup task (measured
        // 2026-10-07). A count, then the paths.
        writer_t gone = { 0 };
        size_t ngone = 0;
        wchar_t *upg2 = get_property(h, L"UPGRADINGPRODUCTCODE");
        if (!machine && remove && _wcsicmp(remove, L"ALL") == 0 && !(upg2 && upg2[0])) {
            MSIHANDLE db = MsiGetActiveDatabase(h), view = 0, rec = 0;
            if (db && MsiDatabaseOpenViewW(db, L"SELECT `File`, `Component_` FROM `File`", &view) == ERROR_SUCCESS && MsiViewExecute(view, 0) == ERROR_SUCCESS) {
                while (ngone < 4096 && gone.len < 20000 && MsiViewFetch(view, &rec) == ERROR_SUCCESS) {        // the data has a size (MAX_TEXT)
                    static wchar_t key[80], comp[80], fmt[96], path[MAX_PATH * 4];
                    DWORD kn = 80, cn = 80, pn = MAX_PATH * 4;
                    INSTALLSTATE have = INSTALLSTATE_UNKNOWN, act = INSTALLSTATE_UNKNOWN;
                    if (MsiRecordGetStringW(rec, 1, key, &kn) == ERROR_SUCCESS && MsiRecordGetStringW(rec, 2, comp, &cn) == ERROR_SUCCESS &&
                        MsiGetComponentStateW(h, comp, &have, &act) == ERROR_SUCCESS && act == INSTALLSTATE_ABSENT && wfmt(fmt, 96, L"[#%ls]", key) > 0) {
                        MSIHANDLE fr = MsiCreateRecord(1);
                        MsiRecordSetStringW(fr, 0, fmt);
                        if (MsiFormatRecordW(h, fr, path, &pn) == ERROR_SUCCESS && path[0]) {
                            w_field(&gone, path);
                            ++ngone;
                        }
                        MsiCloseHandle(fr);
                    }
                    MsiCloseHandle(rec);
                }
            }
            if (view) MsiCloseHandle(view);
            if (db) MsiCloseHandle(db);
        }
        if (upg2) HeapFree(GetProcessHeap(), 0, upg2);
        wfmt(num, 24, L"%zu", ngone);
        w_field(&w, num);
        if (ngone && gone.buf) w_raw(&w, gone.buf, gone.len);
        if (gone.buf) HeapFree(GetProcessHeap(), 0, gone.buf);
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
    // The files a per-user removal took away (a count, then the paths): looked at below.
    if (!r_field(&r, num, 24)) goto done;
    unsigned long ngone = wcstoul(num, NULL, 10);
    const wchar_t *gone_at = r.p;
    for (unsigned long i = 0; i < ngone; ++i) {
        if (!r_field(&r, before, MAX_PATH * 4)) goto done;
    }
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
    // A per-user removal: the product's files that are still there (a program holds them).
    if (!machine) {
        reader_t g = { gone_at, r.end };
        for (unsigned long i = 0; i < ngone && r_field(&g, before, MAX_PATH * 4); ++i) {
            if (GetFileAttributesW(before) == INVALID_FILE_ATTRIBUTES) continue;
            w_text(&files, L"file\t");
            w_text(&files, before);
            w_text(&files, L"\r\n");
            ++found;
            log_line(h, L"rubrapack: cleanup: %ls is still there (held); noted for the cleanup task", before);
        }
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
        // No dialog chose a language: the user's, when the package has it (RP_LANGS: "1042=ko;...").
        wchar_t *ul = get_property(h, L"UserLanguageID"), *map = get_property(h, L"RP_LANGS");
        size_t un = ul ? wcslen(ul) : 0;
        for (const wchar_t *p = map; un && p && *p; p = wcschr(p, L';') ? wcschr(p, L';') + 1 : NULL) {
            if (wcsncmp(p, ul, un) != 0 || p[un] != L'=') continue;
            const wchar_t *code = p + un + 1, *end = wcschr(code, L';');
            size_t cn = end ? (size_t)(end - code) : wcslen(code);
            if (cn == 0 || cn > 15) break;
            wchar_t c[16];
            memcpy(c, code, cn * sizeof *c);
            c[cn] = 0;
            wfmt(name, 80, L"RpPre%ls_%ls", id, c);
            text = get_property(h, name);
            break;
        }
        if (ul) HeapFree(GetProcessHeap(), 0, ul);
        if (map) HeapFree(GetProcessHeap(), 0, map);
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

// ---- wiping at removal -----------------------------------------------------------------------------
//
// [remove] recursive = true and [registry] remove = true, on = "uninstall" (DECISIONS 2026-10-11
// "Wiping at removal"): what the program made itself - a folder with everything in it, a key with
// everything below it - goes when the product is really removed.
//   RpPurgePrepare immediate: nothing when UPGRADINGPRODUCTCODE is set (an upgrade removing this
//                   version). Otherwise, for each plan record of RP_PURGE (d, Directory key, "", "",
//                   component / k, root, view, key, component) whose component is being removed,
//                   one line for a commit action: RP_PurgeMachine (per machine: runs as SYSTEM) or
//                   RP_PurgeUser (runs as the user: everything of a per-user installation, and the
//                   user's profile and HKCU of a per-machine one). A folder that is, or holds, one
//                   of Windows' own folders, or in which another installed product has a component,
//                   is kept.
//   RpPurge         commit: deletes them. A removal that fails has run no commit action, so it has
//                   touched nothing. No link is followed: every folder and file is opened as itself
//                   (FILE_FLAG_OPEN_REPARSE_POINT), checked to be where its name says by its handle,
//                   and deleted by that handle; a link is deleted as a link. What a program holds
//                   stays. Registry keys likewise (REG_OPTION_OPEN_LINK, deleted by handle).
//   Neither fails the installation.

#define PURGE_PATH 32768

NTSYSAPI LONG NTAPI NtDeleteKey(HANDLE key);

static bool starts_with_i(const wchar_t *s, const wchar_t *prefix) {
    size_t n = wcslen(prefix);
    return wcslen(s) >= n && CompareStringOrdinal(s, (int)n, prefix, (int)n, TRUE) == CSTR_EQUAL;
}

// Why folder `dir` (ending in '\') is not wiped, or NULL: it is not a folder on a drive, it is one
// of Windows' own folders or holds one, or it is inside the Windows folder.
static const wchar_t *purge_refused(MSIHANDLE h, const wchar_t *dir) {
    static const wchar_t *const known[] = {
        L"WindowsFolder", L"ProgramFilesFolder", L"ProgramFiles64Folder", L"CommonFilesFolder", L"CommonFiles64Folder", L"CommonAppDataFolder",
        L"AppDataFolder", L"LocalAppDataFolder", L"DesktopFolder", L"PersonalFolder", L"StartMenuFolder", L"ProgramMenuFolder", L"StartupFolder",
        L"FavoritesFolder", L"SendToFolder", L"TemplateFolder", L"FontsFolder", L"SystemFolder", L"System64Folder", L"TempFolder",
        L"MyPicturesFolder", L"AdminToolsFolder", L"%USERPROFILE", L"%PUBLIC", L"%ProgramData", L"%SystemRoot", L"%ProgramFiles", L"%ProgramW6432",
    };
    size_t n = wcslen(dir);
    if (n <= 3 || dir[1] != L':' || dir[2] != L'\\' || dir[n - 1] != L'\\') return L"it is not a folder below a drive's root";
    const wchar_t *why = NULL;
    for (size_t i = 0; i < sizeof known / sizeof known[0] && why == NULL; ++i) {
        static wchar_t path[MAX_PATH * 4];
        path[0] = 0;
        if (known[i][0] == L'%') {
            DWORD got = GetEnvironmentVariableW(known[i] + 1, path, MAX_PATH * 4 - 2);
            if (got == 0 || got >= MAX_PATH * 4 - 2) continue;
            if (path[got - 1] != L'\\') wcscat(path, L"\\");
        } else {
            wchar_t *v = get_property(h, known[i]);
            if (v && wcslen(v) < MAX_PATH * 4) wcscpy(path, v);
            if (v) HeapFree(GetProcessHeap(), 0, v);
        }
        if (path[0] == 0) continue;
        if (starts_with_i(path, dir)) why = L"it is, or holds, one of Windows' own folders";
        else if (i == 0 && starts_with_i(dir, path)) why = L"it is inside the Windows folder";
    }
    return why;
}

// Whether a product other than `self` has a component whose key path is inside `dir`.
static bool other_product_in(const wchar_t *dir, const wchar_t *self, wchar_t found[39]) {
    static wchar_t path[MAX_PATH * 4];
    wchar_t cid[39], prod[39];
    for (DWORD i = 0; MsiEnumComponentsW(i, cid) == ERROR_SUCCESS; ++i) {
        for (DWORD j = 0; MsiEnumClientsW(cid, j, prod) == ERROR_SUCCESS; ++j) {
            if (self && lstrcmpiW(prod, self) == 0) continue;
            DWORD n = MAX_PATH * 4;
            INSTALLSTATE st = MsiGetComponentPathW(prod, cid, path, &n);
            if ((st == INSTALLSTATE_LOCAL || st == INSTALLSTATE_SOURCE) && path[0] && path[1] == L':' && starts_with_i(path, dir)) {
                wcscpy(found, prod);
                return true;
            }
        }
    }
    return false;
}

__declspec(dllexport) UINT __stdcall RpPurgePrepare(MSIHANDLE h) {
    wchar_t *plan = get_property(h, L"RP_PURGE");
    wchar_t *split = get_property(h, L"RP_PURGE_SPLIT");
    wchar_t *upgrading = get_property(h, L"UPGRADINGPRODUCTCODE");
    wchar_t *allusers = get_property(h, L"ALLUSERS");
    wchar_t *self = get_property(h, L"ProductCode");
    bool machine = allusers && allusers[0] == L'1';
    bool two = machine && split && split[0] == L'1';
    writer_t user = { 0 }, sys = { 0 };
    w_raw(&user, L"RPP1", 4);
    w_raw(&sys, L"RPP1", 4);
    if (plan && upgrading && upgrading[0]) {
        log_line(h, L"rubrapack: purge: an upgrade (%ls) removes this version: nothing is wiped", upgrading);
    } else if (plan && wcsncmp(plan, L"RPP1", 4) == 0) {
        static wchar_t profile[MAX_PATH * 4], dir[MAX_PATH * 4];
        DWORD pn = GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH * 4 - 2);
        if (pn == 0 || pn >= MAX_PATH * 4 - 2) profile[0] = 0;
        else if (profile[pn - 1] != L'\\') wcscat(profile, L"\\");
        reader_t r = { plan + 4, plan + wcslen(plan) };
        static wchar_t f[5][MAX_TEXT / 8];
        while (r.p < r.end) {
            bool ok = true;
            for (int i = 0; i < 5; ++i) ok = ok && r_field(&r, f[i], MAX_TEXT / 8);
            if (!ok) break;
            INSTALLSTATE installed = INSTALLSTATE_UNKNOWN, action = INSTALLSTATE_UNKNOWN;
            MsiGetComponentStateW(h, f[4], &installed, &action);
            if (action != INSTALLSTATE_ABSENT) continue;
            if (f[0][0] == L'd') {
                DWORD dn = MAX_PATH * 4;
                if (MsiGetTargetPathW(h, f[1], dir, &dn) != ERROR_SUCCESS) continue;
                wchar_t other[39];
                const wchar_t *why = purge_refused(h, dir);
                if (why) {
                    log_line(h, L"rubrapack: purge: %ls is kept: %ls", dir, why);
                    continue;
                }
                if (other_product_in(dir, self, other)) {
                    log_line(h, L"rubrapack: purge: %ls is kept: the installed product %ls has files there", dir, other);
                    continue;
                }
                writer_t *w = two && !(profile[0] && starts_with_i(dir, profile)) ? &sys : &user;
                dir[wcslen(dir) - 1] = 0;
                w_field(w, L"t"); w_field(w, dir); w_field(w, L""); w_field(w, L"");
                log_line(h, L"rubrapack: purge: folder %ls (%ls)", dir, w == &sys ? L"as SYSTEM" : L"as the user");
            } else if (f[0][0] == L'k') {
                const wchar_t *root = wcscmp(f[1], L"HKMU") == 0 ? (machine ? L"HKLM" : L"HKCU") : f[1];
                if (wcscmp(root, L"HKLM") != 0 && wcscmp(root, L"HKCU") != 0) continue;
                writer_t *w = two && wcscmp(root, L"HKLM") == 0 ? &sys : &user;
                w_field(w, L"k"); w_field(w, root); w_field(w, f[2]); w_field(w, f[3]);
                log_line(h, L"rubrapack: purge: key %ls\\%ls (%ls)", root, f[3], w == &sys ? L"as SYSTEM" : L"as the user");
            }
        }
    }
    set_data(h, L"RP_PurgeUser", &user);
    set_data(h, L"RP_PurgeMachine", &sys);
    wchar_t *all[] = { plan, split, upgrading, allusers, self, user.buf, sys.buf };
    for (size_t i = 0; i < sizeof all / sizeof all[0]; ++i) {
        if (all[i]) HeapFree(GetProcessHeap(), 0, all[i]);
    }
    return ERROR_SUCCESS;
}

typedef struct {
    MSIHANDLE     h;
    wchar_t      *path;         // "\\?\C:\..." of the entry at hand, PURGE_PATH characters
    wchar_t      *real;         // where a handle says it is
    unsigned long gone, left;
} purge_t;

// Opens the entry at p->path as itself and checks, by the handle, that it is where its name says
// (a folder above it put elsewhere by a link gives another place). INVALID_HANDLE_VALUE otherwise.
static HANDLE purge_open(purge_t *p, DWORD *attr) {
    const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    const DWORD flags = FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT;
    HANDLE f = CreateFileW(p->path, DELETE | FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | SYNCHRONIZE, share, NULL,
                           OPEN_EXISTING, flags, NULL);
    if (f == INVALID_HANDLE_VALUE)
        f = CreateFileW(p->path, DELETE | FILE_READ_ATTRIBUTES | SYNCHRONIZE, share, NULL, OPEN_EXISTING, flags, NULL);
    if (f == INVALID_HANDLE_VALUE) return f;
    FILE_ATTRIBUTE_TAG_INFO ti;
    DWORD n = GetFinalPathNameByHandleW(f, p->real, PURGE_PATH, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!GetFileInformationByHandleEx(f, FileAttributeTagInfo, &ti, sizeof ti) || n == 0 || n >= PURGE_PATH ||
        CompareStringOrdinal(p->real, -1, p->path, -1, TRUE) != CSTR_EQUAL) {
        CloseHandle(f);
        SetLastError(ERROR_CANT_RESOLVE_FILENAME);
        return INVALID_HANDLE_VALUE;
    }
    *attr = ti.FileAttributes;
    return f;
}

static bool purge_delete(HANDLE f, DWORD attr) {
    if (attr & FILE_ATTRIBUTE_READONLY) {
        FILE_BASIC_INFO bi;
        memset(&bi, 0, sizeof bi);
        bi.FileAttributes = (attr & ~(DWORD)FILE_ATTRIBUTE_READONLY) ? (attr & ~(DWORD)FILE_ATTRIBUTE_READONLY) : FILE_ATTRIBUTE_NORMAL;
        SetFileInformationByHandle(f, FileBasicInfo, &bi, sizeof bi);
    }
    // The name goes at once even while another program has the file open (Windows 10 1709 and
    // later: class 21, delete | POSIX semantics | ignore read-only); the older way otherwise.
    struct { DWORD flags; } ex = { 0x1 | 0x2 | 0x10 };
    if (SetFileInformationByHandle(f, (FILE_INFO_BY_HANDLE_CLASS)21, &ex, sizeof ex)) return true;
    FILE_DISPOSITION_INFO di = { TRUE };
    return SetFileInformationByHandle(f, FileDispositionInfo, &di, sizeof di) != 0;
}

// Empties the folder open as `d` (p->path, `len` characters long, names it).
static void purge_inside(purge_t *p, HANDLE d, size_t len, int depth) {
    const DWORD cap = 64 * 1024;
    BYTE *buf = HeapAlloc(GetProcessHeap(), 0, cap);
    if (buf == NULL) return;
    bool first = true;
    while (GetFileInformationByHandleEx(d, first ? FileIdBothDirectoryRestartInfo : FileIdBothDirectoryInfo, buf, cap)) {
        first = false;
        for (FILE_ID_BOTH_DIR_INFO *e = (FILE_ID_BOTH_DIR_INFO *)buf;; e = (FILE_ID_BOTH_DIR_INFO *)((BYTE *)e + e->NextEntryOffset)) {
            size_t nl = e->FileNameLength / sizeof(wchar_t);
            bool dots = (nl == 1 && e->FileName[0] == L'.') || (nl == 2 && e->FileName[0] == L'.' && e->FileName[1] == L'.');
            if (!dots && len + 1 + nl < PURGE_PATH) {
                p->path[len] = L'\\';
                memcpy(p->path + len + 1, e->FileName, nl * sizeof(wchar_t));
                p->path[len + 1 + nl] = 0;
                DWORD attr = 0;
                HANDLE f = purge_open(p, &attr);
                bool ok = f != INVALID_HANDLE_VALUE;
                if (ok) {
                    // A real folder is emptied first; a link, whatever it points at, goes as it is.
                    if ((attr & FILE_ATTRIBUTE_DIRECTORY) && !(attr & FILE_ATTRIBUTE_REPARSE_POINT) && depth < 128) {
                        purge_inside(p, f, len + 1 + nl, depth + 1);
                        p->path[len + 1 + nl] = 0;
                    }
                    ok = purge_delete(f, attr);
                }
                DWORD err = ok ? 0 : GetLastError();
                if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
                if (ok) ++p->gone;
                else {
                    ++p->left;
                    if (p->left <= 20) log_line(p->h, L"rubrapack: purge: %ls stays (%lu)", p->path + 4, (unsigned long)err);
                }
            } else if (!dots) {
                ++p->left;
            }
            if (e->NextEntryOffset == 0) break;
        }
    }
    p->path[len] = 0;
    HeapFree(GetProcessHeap(), 0, buf);
}

static void purge_tree(MSIHANDLE h, const wchar_t *dir) {
    purge_t p = { h, HeapAlloc(GetProcessHeap(), 0, PURGE_PATH * sizeof(wchar_t)), HeapAlloc(GetProcessHeap(), 0, PURGE_PATH * sizeof(wchar_t)), 0, 0 };
    size_t len = wcslen(dir);
    if (p.path && p.real && len + 8 < PURGE_PATH && len > 3 && dir[1] == L':') {
        wcscpy(p.path, L"\\\\?\\");
        wcscat(p.path, dir);
        DWORD attr = 0;
        HANDLE d = purge_open(&p, &attr);
        if (d == INVALID_HANDLE_VALUE) {
            DWORD e = GetLastError();
            if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) log_line(h, L"rubrapack: purge: %ls is not there", dir);
            else log_line(h, L"rubrapack: purge: %ls is kept: it cannot be opened as the folder its name says (%lu)", dir, (unsigned long)e);
        } else if (!(attr & FILE_ATTRIBUTE_DIRECTORY) || (attr & FILE_ATTRIBUTE_REPARSE_POINT)) {
            log_line(h, L"rubrapack: purge: %ls is kept: it is a link or not a folder", dir);
            CloseHandle(d);
        } else {
            purge_inside(&p, d, len + 4, 0);
            bool ok = p.left == 0 && purge_delete(d, attr);
            CloseHandle(d);
            log_line(h, L"rubrapack: purge: %ls: %lu deleted, %lu left, the folder %ls", dir, p.gone, p.left, ok ? L"removed" : L"stays");
        }
    }
    if (p.path) HeapFree(GetProcessHeap(), 0, p.path);
    if (p.real) HeapFree(GetProcessHeap(), 0, p.real);
}

// Deletes the key `name` below `parent` with everything below it; a link is deleted as a link.
static LSTATUS purge_subkey(HKEY parent, const wchar_t *name, REGSAM view, int depth) {
    HKEY k;
    LSTATUS ls = RegOpenKeyExW(parent, name, REG_OPTION_OPEN_LINK, DELETE | KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE | view, &k);
    if (ls != ERROR_SUCCESS) return ls;
    DWORD type = 0;
    bool link = RegQueryValueExW(k, L"SymbolicLinkValue", NULL, &type, NULL, NULL) == ERROR_SUCCESS && type == REG_LINK;
    if (!link && depth < 128) {
        wchar_t child[256];
        DWORD at = 0;
        for (;;) {
            DWORD cn = 256;
            LSTATUS es = RegEnumKeyExW(k, at, child, &cn, NULL, NULL, NULL, NULL);
            if (es != ERROR_SUCCESS) break;
            if (purge_subkey(k, child, view, depth + 1) != ERROR_SUCCESS) ++at;     // it stays: go past it
        }
    }
    LONG st = NtDeleteKey(k);
    RegCloseKey(k);
    return st == 0 ? ERROR_SUCCESS : ERROR_ACCESS_DENIED;
}

static void purge_key(MSIHANDLE h, const wchar_t *root, const wchar_t *view, const wchar_t *key) {
    HKEY base = HKEY_LOCAL_MACHINE;
    bool user = wcscmp(root, L"HKCU") == 0;
    // The hive of the user this action runs as, not the process's.
    if (user && RegOpenCurrentUser(KEY_READ, &base) != ERROR_SUCCESS) {
        log_line(h, L"rubrapack: purge: key %ls\\%ls: the user's registry cannot be opened", root, key);
        return;
    }
    LSTATUS ls = purge_subkey(base, key, view_flag(view), 0);
    if (user) RegCloseKey(base);
    log_line(h, L"rubrapack: purge: key %ls\\%ls: %ls (%ld)", root, key,
             ls == ERROR_SUCCESS ? L"removed" : ls == ERROR_FILE_NOT_FOUND ? L"not there" : L"stays", (long)ls);
}

__declspec(dllexport) UINT __stdcall RpPurge(MSIHANDLE h) {
    wchar_t *data = get_property(h, L"CustomActionData");
    if (data == NULL) return ERROR_SUCCESS;
    if (wcsncmp(data, L"RPP1", 4) == 0) {
        reader_t r = { data + 4, data + wcslen(data) };
        static wchar_t f[4][MAX_PATH * 4];
        while (r.p < r.end && r_field(&r, f[0], 8) && r_field(&r, f[1], MAX_PATH * 4) && r_field(&r, f[2], 8) && r_field(&r, f[3], MAX_PATH * 4)) {
            if (f[0][0] == L't') purge_tree(h, f[1]);
            else if (f[0][0] == L'k') purge_key(h, f[1], f[2], f[3]);
        }
    }
    HeapFree(GetProcessHeap(), 0, data);
    return ERROR_SUCCESS;
}

// ---- the question an installed product is met with ---------------------------------------------------
//
// RpMaintain, immediate, in the UI sequence of a package without dialogs (ui = "none"), when the
// product is already installed and nothing was chosen on the command line: Repair, Remove or
// Cancel, in a window of its own; Remove is asked once more (Yes / No). The dialog sets have pages
// for this (RpMaintenanceDlg, RpRemoveDlg). The
// choice is what the command line would have said: REINSTALL=ALL REINSTALLMODE=ecmus, or REMOVE=ALL.
// Texts: the RpPreMaint* properties, as the preflight's.

#include "rp_ask.h"

static wchar_t *maint_text(MSIHANDLE h, const wchar_t *id) {
    wchar_t *text = pre_text(h, id);
    if (text == NULL) return NULL;
    MSIHANDLE rec = MsiCreateRecord(1);
    MsiRecordSetStringW(rec, 0, text);
    DWORD n = 0;
    wchar_t *out = NULL;
    if (MsiFormatRecordW(h, rec, L"", &n) == ERROR_MORE_DATA && (out = HeapAlloc(GetProcessHeap(), 0, ((size_t)n + 1) * sizeof *out)) != NULL) {
        ++n;
        if (MsiFormatRecordW(h, rec, out, &n) != ERROR_SUCCESS) out[0] = 0;
    }
    MsiCloseHandle(rec);
    if (out == NULL) return text;
    HeapFree(GetProcessHeap(), 0, text);
    return out;
}

__declspec(dllexport) UINT __stdcall RpMaintain(MSIHANDLE h) {
    wchar_t *t[6];
    static const wchar_t *const ids[6] = { L"MaintTitle", L"MaintText", L"Repair", L"Remove", L"Cancel", L"RemoveAsk" };
    for (size_t i = 0; i < 6; ++i) t[i] = maint_text(h, ids[i]);
    rp_ask_t a = { t[0] ? t[0] : L"", t[1] ? t[1] : L"", t[2] ? t[2] : L"Repair", t[3] ? t[3] : L"Remove", t[4] ? t[4] : L"Cancel" };
    int r;
    for (;;) {
        r = rp_ask(NULL, &a);       // no owner: the window in front may be anyone's
        // Remove is asked once more (No is the default button, and leads back to the question).
        if (r != RP_ASK_SECOND || MessageBoxW(NULL, t[5] ? t[5] : L"Remove?", a.title,
                                              MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2 | MB_TOPMOST | MB_SETFOREGROUND) == IDYES) break;
    }
    for (size_t i = 0; i < 6; ++i) {
        if (t[i]) HeapFree(GetProcessHeap(), 0, t[i]);
    }
    if (r == RP_ASK_FIRST) {
        MsiSetPropertyW(h, L"REINSTALL", L"ALL");
        MsiSetPropertyW(h, L"REINSTALLMODE", L"ecmus");
        log_line(h, L"rubrapack: maintain: repair");
    } else if (r == RP_ASK_SECOND) {
        MsiSetPropertyW(h, L"REMOVE", L"ALL");
        log_line(h, L"rubrapack: maintain: remove");
    } else {
        log_line(h, L"rubrapack: maintain: cancelled");
        return ERROR_INSTALL_USEREXIT;
    }
    return ERROR_SUCCESS;
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

// ---- the details box of the progress page (DECISIONS 2026-10-13) -------------------------------------
//
// RpDetails, immediate and asynchronous, in the UI sequence after the progress page is up and
// before ExecuteAction: while the installation runs it reads the log of this run as it grows
// (MsiLogFileLocation; MsiLogging has the flush flag) and puts lines into the progress page's
// scrolling box - a ScrollableText control of the dialog, which lives in the client's process and
// is written to with the edit control's own messages. By default the lines are the readable ones:
// each action Windows Installer starts (its description, in the dialog's language) and each thing
// done under it (a file copied, a value written, a shortcut made). With the page's check box
// ticked the box holds the log's lines as they are.
// It ends when the log says ExecuteAction has ended, when RpDetailsDone (after ExecuteAction) sets
// its event, when the box is gone, or when nothing was logged for three seconds and no installation
// holds Windows Installer's execution mutex (after the service was seen at work). It never fails the installation: without a log, a
// box or a readable line, the page simply shows the progress bar as before.

typedef struct {
    const wchar_t *title;
    HWND dlg, edit, check;
} det_win_t;

static BOOL CALLBACK det_child(HWND w, LPARAM lp) {
    det_win_t *d = (det_win_t *)lp;
    wchar_t cls[32];
    if (GetClassNameW(w, cls, 32) == 0) return TRUE;
    LONG style = GetWindowLongW(w, GWL_STYLE);
    if (_wcsnicmp(cls, L"RichEdit", 8) == 0 && d->edit == NULL) d->edit = w;
    else if (_wcsicmp(cls, L"Button") == 0 && ((style & 0xF) == BS_AUTOCHECKBOX || (style & 0xF) == BS_CHECKBOX)) d->check = w;
    return TRUE;
}

static BOOL CALLBACK det_top(HWND w, LPARAM lp) {
    det_win_t *d = (det_win_t *)lp;
    wchar_t cls[40], title[300];
    if (!IsWindowVisible(w) || GetClassNameW(w, cls, 40) == 0 || _wcsnicmp(cls, L"MsiDialog", 9) != 0) return TRUE;
    title[0] = 0;
    GetWindowTextW(w, title, 300);
    if (d->title && d->title[0] && wcscmp(title, d->title) != 0) return TRUE;
    d->edit = d->check = NULL;
    EnumChildWindows(w, det_child, lp);
    if (d->edit == NULL) return TRUE;
    d->dlg = w;
    return FALSE;
}

static LRESULT det_send(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    DWORD_PTR r = 0;
    SendMessageTimeoutW(w, m, wp, lp, SMTO_ABORTIFHUNG, 2000, &r);
    return (LRESULT)r;
}

// The value of `key` in an operation's arguments ("A=1,B=two,C=3"): up to the next ",Name=" or the end.
static bool det_arg(const wchar_t *args, const wchar_t *key, wchar_t *out, size_t cap) {
    size_t kn = wcslen(key);
    for (const wchar_t *p = args; p && *p; p = wcschr(p, L',') ? wcschr(p, L',') + 1 : NULL) {
        if (wcsncmp(p, key, kn) != 0 || p[kn] != L'=') continue;
        const wchar_t *v = p + kn + 1, *e = v;
        for (;; ++e) {
            if (*e == 0) break;
            if (*e != L',') continue;
            const wchar_t *q = e + 1;           // a comma ends the value when a name and '=' follow, or another comma
            while ((*q >= L'A' && *q <= L'Z') || (*q >= L'a' && *q <= L'z') || (*q >= L'0' && *q <= L'9')) ++q;
            if ((q > e + 1 && *q == L'=') || e[1] == L',' || e[1] == 0) break;
        }
        size_t n = (size_t)(e - v);
        if (n >= cap) n = cap - 1;
        memcpy(out, v, n * sizeof *out);
        out[n] = 0;
        return n > 0;
    }
    return false;
}

enum { DET_OPS = 17 };

typedef struct {
    wchar_t *label[DET_OPS];        // each operation's label in the dialog's language (RpDet_<op>_<code>), or NULL
    wchar_t  folder[520], key[520], ini[520];
} det_state_t;

// The script's operations that have a readable line: the argument that names the thing, and under
// what it stands (1 the folder, 2 the registry key, 3 the INI file). The label is the package's
// (RpDet_<op>_<language>, from the language's pack), English here when it has none.
typedef struct {
    const wchar_t *op, *arg, *en;
    int            where;
} det_op_t;

static const det_op_t det_ops[DET_OPS] = {
    { L"FileCopy", L"DestName", L"Copy file", 1 },
    { L"FileRemove", L"FileName", L"Remove file", 1 },
    { L"FilePatch", L"TargetName", L"Patch file", 1 },
    { L"FolderCreate", L"Folder", L"Create folder", 0 },
    { L"FolderRemove", L"Folder", L"Remove folder", 0 },
    { L"RegAddValue", L"Name", L"Write registry value", 2 },
    { L"RegRemoveValue", L"Name", L"Remove registry value", 2 },
    { L"RegRemoveKey", NULL, L"Remove registry key", 2 },
    { L"ShortcutCreate", L"Name", L"Create shortcut", 1 },
    { L"ShortcutRemove", L"Name", L"Remove shortcut", 1 },
    { L"ServiceInstall", L"Name", L"Install service", 0 },
    { L"ServiceControl", L"Name", L"Control service", 0 },
    { L"IniWriteRemoveValue", L"Key", L"Write INI value", 3 },
    { L"UpdateEnvironmentStrings", L"Name", L"Set environment variable", 0 },
    { L"RegisterFont", L"Title", L"Register font", 0 },
    { L"UnregisterFont", L"Title", L"Unregister font", 0 },
    { L"CustomActionSchedule", L"Action", L"Run action", 0 },
};

// One log line as a readable line in `out` (empty: nothing to show for it).
static void det_readable(det_state_t *st, const wchar_t *line, wchar_t *out, size_t cap) {
    const det_op_t *ops = det_ops;
    out[0] = 0;
    const wchar_t *own = wcsstr(line, L"rubrapack: ");
    if (own) {                              // the helper's own lines (what was closed, kept, wiped), not its housekeeping
        if (wcsncmp(own + 11, L"cleanup:", 8) != 0 && wcsncmp(own + 11, L"details:", 8) != 0) wfmt(out, cap, L"  %.900ls", own + 11);
        return;
    }
    const wchar_t *op = wcsstr(line, L"Executing op: ");
    if (op == NULL) return;
    op += 14;
    const wchar_t *paren = wcschr(op, L'(');
    if (paren == NULL) return;
    size_t on = (size_t)(paren - op);
    static wchar_t args[4096], v[1024];
    size_t an = wcslen(paren + 1);
    if (an >= 4096) an = 4095;
    memcpy(args, paren + 1, an * sizeof *args);
    args[an] = 0;
    if (an && args[an - 1] == L')') args[an - 1] = 0;
    if (on == 11 && wcsncmp(op, L"ActionStart", 11) == 0) {
        if (det_arg(args, L"Description", v, 1024)) wfmt(out, cap, L"%.900ls", v);
        return;
    }
    if (on == 15 && wcsncmp(op, L"SetTargetFolder", 15) == 0) {
        if (!det_arg(args, L"Folder", st->folder, 520)) st->folder[0] = 0;
        // A shell folder is given by its number ("23\"): its path, when the system has one.
        wchar_t *end = NULL;
        long csidl = wcstol(st->folder, &end, 10);
        static wchar_t shell[MAX_PATH], rest[520];
        if (end != st->folder && (*end == L'\\' || *end == 0) && csidl >= 0 && csidl < 256 && SHGetFolderPathW(NULL, (int)csidl, NULL, 0, shell) == S_OK) {
            wfmt(rest, 520, L"%ls", end);
            wfmt(st->folder, 520, L"%ls%ls", shell, rest);
        }
        return;
    }
    if (on == 10 && wcsncmp(op, L"RegOpenKey", 10) == 0) {
        static wchar_t root[40], key[480];
        if (!det_arg(args, L"Key", key, 480)) key[0] = 0;
        if (!det_arg(args, L"Root", root, 40)) root[0] = 0;
        long r = wcstol(root, NULL, 10);
        const wchar_t *hive = r == (long)0x80000000 ? L"HKCR\\" : r == (long)0x80000001 ? L"HKCU\\" : r == (long)0x80000002 ? L"HKLM\\" : r == (long)0x80000003 ? L"HKU\\" : L"";
        wfmt(st->key, 520, L"%ls%ls", hive, key);
        return;
    }
    if (on == 11 && wcsncmp(op, L"IniFilePath", 11) == 0) { if (!det_arg(args, L"File", st->ini, 520)) st->ini[0] = 0; return; }
    for (size_t i = 0; i < DET_OPS; ++i) {
        if (wcslen(ops[i].op) != on || wcsncmp(op, ops[i].op, on) != 0) continue;
        v[0] = 0;
        if (ops[i].arg) det_arg(args, ops[i].arg, v, 1024);
        const wchar_t *shown_name = v;
        wchar_t *bar = wcschr(v, L'|');                     // "short|long": the long name
        if (bar && ops[i].where == 1) shown_name = bar + 1;
        if (wcscmp(ops[i].op, L"CustomActionSchedule") == 0) {
            // The source's own actions are RP_<ID>_Do / RP_<ID>_Undo; the rest of rubrapack's (and
            // every rollback twin) is housekeeping.
            size_t vn = wcslen(v);
            if (vn > 8 && wcscmp(v + vn - 8, L"Rollback") == 0) return;
            if (wcsncmp(v, L"RP_", 3) == 0) {
                bool is_do = vn > 6 && wcscmp(v + vn - 3, L"_Do") == 0, is_undo = vn > 8 && wcscmp(v + vn - 5, L"_Undo") == 0;
                if (!is_do && !is_undo) return;
                v[vn - (is_do ? 3 : 5)] = 0;
                shown_name = v + 3;
            }
        }
        const wchar_t *base = ops[i].where == 1 ? st->folder : ops[i].where == 2 ? st->key : ops[i].where == 3 ? st->ini : L"";
        if (ops[i].where == 1 && ((shown_name[0] && shown_name[1] == L':') || (shown_name[0] == L'\\' && shown_name[1] == L'\\'))) base = L"";    // a whole path already (a rollback)
        size_t bn = wcslen(base);
        const wchar_t *sep = bn && shown_name[0] && base[bn - 1] != L'\\' ? (ops[i].where == 3 ? L": " : L"\\") : L"";
        wfmt(out, cap, L"  %.80ls: %.500ls%ls%.380ls", st->label[i] && st->label[i][0] ? st->label[i] : ops[i].en, base, sep, shown_name);
        return;
    }
}

// Whether an installation is running its script (Windows Installer's execution mutex is held).
static bool det_executing(void) {
    HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, L"Global\\_MSIExecute");
    if (m == NULL) return false;
    DWORD r = WaitForSingleObject(m, 0);
    if (r == WAIT_OBJECT_0 || r == WAIT_ABANDONED) ReleaseMutex(m);
    CloseHandle(m);
    return r == WAIT_TIMEOUT;
}

static void det_event_name(MSIHANDLE h, wchar_t name[80]) {
    wchar_t *code = get_property(h, L"ProductCode");
    wfmt(name, 80, L"Local\\rubrapack-details-%.38ls", code && is_guid(code) ? code : L"x");
    if (code) HeapFree(GetProcessHeap(), 0, code);
}

__declspec(dllexport) UINT __stdcall RpDetailsDone(MSIHANDLE h) {
    wchar_t name[80];
    det_event_name(h, name);
    HANDLE e = OpenEventW(EVENT_MODIFY_STATE, FALSE, name);
    if (e) {
        SetEvent(e);
        CloseHandle(e);
    }
    return ERROR_SUCCESS;
}

__declspec(dllexport) UINT __stdcall RpDetails(MSIHANDLE h) {
    wchar_t *log = get_property(h, L"MsiLogFileLocation"), *lang = get_property(h, L"RPLANGUAGE");
    wchar_t *product = get_property(h, L"ProductName");
    wchar_t evname[80];
    det_event_name(h, evname);
    HANDLE done = CreateEventW(NULL, TRUE, FALSE, evname);
    HANDLE f = INVALID_HANDLE_VALUE;
    det_win_t win = { .title = product };
    static det_state_t st;
    memset(&st, 0, sizeof st);
    for (size_t i = 0; i < DET_OPS; ++i) {
        wchar_t name[96];
        st.label[i] = NULL;
        if (lang && lang[0] && wfmt(name, 96, L"RpDet_%ls_%.15ls", det_ops[i].op, lang) > 0) st.label[i] = get_property(h, name);
    }
    // The whole log so far as text (raw), and how much of it the box has: both views are made from it.
    size_t cap = 1u << 16, len = 0, shown = 0, bytes_cap = 1u << 16, carry = 0;
    wchar_t *raw = HeapAlloc(GetProcessHeap(), 0, cap * sizeof *raw);
    uint8_t *bytes = HeapAlloc(GetProcessHeap(), 0, bytes_cap + 2);
    bool utf16 = false, first = true, full = false, started = false, seen = false, ended = false;
    DWORD quiet_since = GetTickCount(), born = GetTickCount();
    if (log == NULL || log[0] == 0 || raw == NULL || bytes == NULL) goto out;
    for (int i = 0; i < 100 && win.dlg == NULL; ++i) {         // the progress page, up to ten seconds
        EnumWindows(det_top, (LPARAM)&win);
        if (win.dlg == NULL) Sleep(100);
    }
    if (win.dlg == NULL) goto out;
    f = CreateFileW(log, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        log_line(h, L"rubrapack: details: the log cannot be opened (%lu)", (unsigned long)GetLastError());
        goto out;
    }
    det_send(win.edit, WM_USER + 53, 0, 16 * 1024 * 1024);      // EM_EXLIMITTEXT: the box takes more than 64 KB
    while (!ended) {
        // What the log has gained, as text.
        DWORD got = 0;
        bool grew = false;
        while (ReadFile(f, bytes + carry, (DWORD)(bytes_cap - carry), &got, NULL) && got) {
            size_t n = carry + got, at = 0;
            carry = 0;
            if (first) {
                utf16 = n >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE;
                at = utf16 ? 2 : 0;
                first = false;
            }
            size_t units;
            if (utf16) {
                units = (n - at) / 2;
                carry = (n - at) % 2;
            } else {
                units = n - at;
            }
            if (len + units + 1 > cap) {
                size_t ncap = cap * 2 > len + units + 1 ? cap * 2 : len + units + 1;
                wchar_t *nr = ncap <= (48u << 20) ? HeapReAlloc(GetProcessHeap(), 0, raw, ncap * sizeof *raw) : NULL;
                if (nr == NULL) { ended = true; break; }
                raw = nr;
                cap = ncap;
            }
            if (utf16) memcpy(raw + len, bytes + at, units * 2);
            else units = (size_t)MultiByteToWideChar(CP_ACP, 0, (const char *)bytes + at, (int)units, raw + len, (int)(cap - len));
            len += units;
            if (carry) bytes[0] = bytes[n - 1];
            grew = true;
        }
        // The check box: the other view is made from the start.
        bool want_full = win.check && det_send(win.check, BM_GETCHECK, 0, 0) == BST_CHECKED;
        if (want_full != full) {
            full = want_full;
            shown = 0;
            memset(st.folder, 0, sizeof st.folder);
            det_send(win.edit, WM_SETTEXT, 0, (LPARAM)L"");
            grew = true;
        }
        if (grew) {
            // Whole lines only; the views' text for them, appended in one go.
            size_t end = len;
            while (end > shown && raw[end - 1] != L'\n') --end;
            size_t ocap = (end - shown) + 4096, o = 0;
            wchar_t *add = HeapAlloc(GetProcessHeap(), 0, ocap * sizeof *add);
            static wchar_t line[8192], nice[1024];
            for (size_t p = shown; add && p < end;) {
                size_t e = p;
                while (e < end && raw[e] != L'\n') ++e;
                size_t n = e - p;
                if (n && raw[p + n - 1] == L'\r') --n;
                if (n > 8191) n = 8191;
                memcpy(line, raw + p, n * sizeof *line);
                line[n] = 0;
                p = e + 1;
                // The end of the run: the line that says ExecuteAction has ended - after the
                // service's lines, its name with something behind it ("Return value 1."), whatever
                // language Windows writes the rest in.
                if (wcsstr(line, L"MSI (s)")) started = true;
                const wchar_t *ea = started ? wcsstr(line, L": ExecuteAction. ") : NULL;
                if (ea && ea[17] > L' ') ended = true;
                const wchar_t *text = line;
                if (!full) {
                    det_readable(&st, line, nice, 1024);
                    text = nice;
                }
                size_t tn = wcslen(text);
                if (tn == 0 || o + tn + 3 > ocap) continue;
                memcpy(add + o, text, tn * sizeof *add);
                o += tn;
                add[o++] = L'\r';
                add[o++] = L'\n';
            }
            if (add && o) {
                add[o] = 0;
                det_send(win.edit, EM_SETREADONLY, FALSE, 0);
                det_send(win.edit, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
                det_send(win.edit, EM_REPLACESEL, FALSE, (LPARAM)add);
                det_send(win.edit, EM_SETREADONLY, TRUE, 0);
                det_send(win.edit, WM_VSCROLL, SB_BOTTOM, 0);
            }
            if (add) HeapFree(GetProcessHeap(), 0, add);
            shown = end;
            quiet_since = GetTickCount();
        }
        if (ended) break;
        if (WaitForSingleObject(done ? done : GetCurrentProcess(), 150) == WAIT_OBJECT_0 && done) break;
        if (!IsWindow(win.edit)) break;
        // A failed or cancelled run may say nothing more (and a log without the flush flag, one
        // named with /l, says its last lines late): once the service was at work - by its lines or
        // by the execution mutex - quiet and a free mutex mean the run is over.
        DWORD now = GetTickCount();
        bool executing = det_executing();
        if (executing) seen = true;
        if (now - quiet_since > 3000 && (started || seen) && !executing) break;
        if (now - quiet_since > 60000 && !started && !seen) break;                  // nothing ever ran
        if (now - quiet_since > 600000 || now - born > 6u * 3600 * 1000) break;
    }
out:
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    if (done) CloseHandle(done);
    if (raw) HeapFree(GetProcessHeap(), 0, raw);
    if (bytes) HeapFree(GetProcessHeap(), 0, bytes);
    for (size_t i = 0; i < DET_OPS; ++i) {
        if (st.label[i]) HeapFree(GetProcessHeap(), 0, st.label[i]);
    }
    if (log) HeapFree(GetProcessHeap(), 0, log);
    if (lang) HeapFree(GetProcessHeap(), 0, lang);
    if (product) HeapFree(GetProcessHeap(), 0, product);
    return ERROR_SUCCESS;
}

// ---- the install folder page: a folder that holds something (DECISIONS 2026-10-13) --------------------
//
// RpDirCheck, immediate, called by the folder page's Next of a package that wipes its install folder
// at removal: RP_DIR_NOTEMPTY = "1" when the folder the user chose (the property RP_DIRCHECK names)
// exists and has anything in it, else empty. The page then asks before going on.
__declspec(dllexport) UINT __stdcall RpDirCheck(MSIHANDLE h) {
    wchar_t *prop = get_property(h, L"RP_DIRCHECK");
    wchar_t *dir = prop && prop[0] ? get_property(h, prop) : NULL;
    bool holds = false;
    static wchar_t pattern[MAX_PATH * 4];
    if (dir && dir[0] && wfmt(pattern, MAX_PATH * 4, L"%ls%ls*", dir, dir[wcslen(dir) - 1] == L'\\' ? L"" : L"\\") > 0) {
        WIN32_FIND_DATAW fd;
        HANDLE f = FindFirstFileW(pattern, &fd);
        if (f != INVALID_HANDLE_VALUE) {
            do {
                holds = wcscmp(fd.cFileName, L".") != 0 && wcscmp(fd.cFileName, L"..") != 0;
            } while (!holds && FindNextFileW(f, &fd));
            FindClose(f);
        }
    }
    MsiSetPropertyW(h, L"RP_DIR_NOTEMPTY", holds ? L"1" : L"");
    if (prop) HeapFree(GetProcessHeap(), 0, prop);
    if (dir) HeapFree(GetProcessHeap(), 0, dir);
    return ERROR_SUCCESS;
}
