// src/menu/rubrapack_menu.c - the Explorer menu part: one COM server (IExplorerCommand) for every
// [menu.*] item of a package, so a package's author writes no code for the Windows 11 context menu.
//
// Windows 11 shows in its context menu only the commands a package with identity declares
// (desktop4/desktop5 fileExplorerContextMenus, each a COM class). The package - an MSIX, or the
// small "sparse" package an MSI registers for its install folder - names this DLL as the class's
// server (com:SurrogateServer: it runs in dllhost.exe, never inside Explorer). What a class does is
// read from the registry, where the installation wrote it:
//
//   HKLM (or HKCU, per user) \Software\rubrapack\Menu\{CLSID}
//     Text        the item's text;  Text.<lang> for a language ("ko", "ja", "de", ...; the user's
//                 display language picks one, Text is the fallback)
//     Icon        "path" or "path,index" (optional)
//     Command     the program (a full path; in an MSIX "%PKG%\..." = the package's folder)
//     Args        its arguments: %1 (or %V) = the selected path, put in as it is (the author writes
//                 the quotes: "%1", as in a classic verb); %* = every selected path, each quoted
//                 here. Without either, the paths are appended, quoted.
//     Multi       "each" (default: one run per selected item), "one" (one run with all of them),
//                 "single" (the item shows only when one thing is selected)
//     Items       REG_MULTI_SZ: the names of sub keys, each a sub-item with the same values
//                 (then this key has no Command: it is a sub-menu)
//
// Nothing here is read from the selected files, and nothing is run but the Command the
// installation wrote (HKLM needs an administrator to change; HKCU is the user's own).
// Built by `nob parts` for x64, x86 and arm64; Windows only; needs only the system DLLs.

#define COBJMACROS
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>

enum { TEXT_CAP = 1024, PATH_CAP = 32768, MAX_ITEMS = 64, MAX_RUNS = 64 };

static LONG live;                   // objects and locks: DllCanUnloadNow
static HMODULE self;

#define MENU_KEY L"Software\\rubrapack\\Menu\\"

// With the value HKCU\Software\rubrapack\MenuLog (a file's path; set by hand, never by a package)
// each start of a program is appended to that file: the command line and whether it started.
static void say(const wchar_t *what, const wchar_t *text, DWORD code) {
    wchar_t path[MAX_PATH];
    DWORD type = 0, size = sizeof path - sizeof(wchar_t);
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\rubrapack", 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS) return;
    LONG e = RegQueryValueExW(k, L"MenuLog", NULL, &type, (BYTE *)path, &size);
    RegCloseKey(k);
    if (e != ERROR_SUCCESS || type != REG_SZ || size < 2 * sizeof(wchar_t)) return;
    path[size / sizeof(wchar_t)] = 0;
    HANDLE f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    wchar_t num[32];
    DWORD put = 0;
    wsprintfW(num, L" (%lu)\r\n", (unsigned long)code);
    WriteFile(f, what, (DWORD)(wcslen(what) * sizeof(wchar_t)), &put, NULL);
    WriteFile(f, text, (DWORD)(wcslen(text) * sizeof(wchar_t)), &put, NULL);
    WriteFile(f, num, (DWORD)(wcslen(num) * sizeof(wchar_t)), &put, NULL);
    CloseHandle(f);
}

// ---- the registry ----------------------------------------------------------------------------

// Opens <root>\Software\rubrapack\Menu\<path>, HKLM first, then HKCU, in this DLL's own registry view
// (a 32-bit package wrote there what its 32-bit DLL reads).
static bool open_item(const wchar_t *path, HKEY *out) {
    wchar_t key[512];
    if (wcslen(path) + wcslen(MENU_KEY) >= 512) return false;
    wcscpy(key, MENU_KEY);
    wcscat(key, path);
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key, 0, KEY_QUERY_VALUE, out) == ERROR_SUCCESS) return true;
    return RegOpenKeyExW(HKEY_CURRENT_USER, key, 0, KEY_QUERY_VALUE, out) == ERROR_SUCCESS;
}

// A string value (REG_SZ or REG_EXPAND_SZ, taken as written), NUL-terminated; false when absent or too long.
static bool value(HKEY k, const wchar_t *name, wchar_t *out, DWORD cap) {
    DWORD type = 0, size = (cap - 1) * sizeof(wchar_t);
    if (RegQueryValueExW(k, name, NULL, &type, (BYTE *)out, &size) != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) return false;
    out[size / sizeof(wchar_t)] = 0;
    return true;
}

// The language code of the user's display language, as the sources write it ("ko", "pt-br", ...).
static void ui_language(wchar_t out[LOCALE_NAME_MAX_LENGTH]) {
    out[0] = 0;
    ULONG n = 0, cap = 0;
    if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &n, NULL, &cap) && cap > 1) {
        wchar_t *list = HeapAlloc(GetProcessHeap(), 0, cap * sizeof *list);
        if (list && GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &n, list, &cap) && wcslen(list) < LOCALE_NAME_MAX_LENGTH) wcscpy(out, list);
        if (list) HeapFree(GetProcessHeap(), 0, list);
    }
    CharLowerW(out);
}

// Text.<language>, then Text.<language without its region>, then Text.
static bool item_text(HKEY k, wchar_t *out, DWORD cap) {
    wchar_t lang[LOCALE_NAME_MAX_LENGTH], name[LOCALE_NAME_MAX_LENGTH + 8];
    ui_language(lang);
    for (int pass = 0; pass < 2 && lang[0]; ++pass) {
        wcscpy(name, L"Text.");
        wcscat(name, lang);
        if (value(k, name, out, cap)) return true;
        wchar_t *dash = wcschr(lang, L'-');
        if (dash == NULL) break;
        *dash = 0;
    }
    return value(k, L"Text", out, cap);
}

// "%PKG%" at the start of a path is the package's folder in an MSIX: the nearest folder above
// this DLL that holds AppxManifest.xml.
static bool resolve(const wchar_t *in, wchar_t *out, size_t cap) {
    if (wcsncmp(in, L"%PKG%", 5) != 0) {
        if (wcslen(in) >= cap) return false;
        wcscpy(out, in);
        return true;
    }
    DWORD n = GetModuleFileNameW(self, out, (DWORD)cap);
    if (n == 0 || n >= cap) return false;
    for (int up = 0; up < 16; ++up) {
        wchar_t *slash = wcsrchr(out, L'\\');
        if (slash == NULL || slash - out + 20 >= (ptrdiff_t)cap) return false;
        wcscpy(slash, L"\\AppxManifest.xml");
        DWORD a = GetFileAttributesW(out);
        *slash = 0;
        if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (wcslen(out) + wcslen(in + 5) >= cap) return false;
        wcscat(out, in + 5);
        return true;
    }
    return false;
}

// ---- the command ------------------------------------------------------------------------------

typedef struct {
    IExplorerCommand     cmd;
    IEnumExplorerCommand en;        // the same object enumerates its own sub-items
    LONG                 refs;
    wchar_t              path[400]; // below Menu\: "{CLSID}" or "{CLSID}\<sub item>"
    wchar_t              subs[MAX_ITEMS][64];
    ULONG                nsubs, at;
} item_t;

static item_t *item_new(const wchar_t *path);

static item_t *from_cmd(IExplorerCommand *p) { return (item_t *)((char *)p - offsetof(item_t, cmd)); }
static item_t *from_en(IEnumExplorerCommand *p) { return (item_t *)((char *)p - offsetof(item_t, en)); }

static HRESULT item_query(item_t *it, REFIID riid, void **out) {
    if (out == NULL) return E_POINTER;
    *out = NULL;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IExplorerCommand)) *out = &it->cmd;
    else if (IsEqualIID(riid, &IID_IEnumExplorerCommand)) *out = &it->en;
    else return E_NOINTERFACE;
    InterlockedIncrement(&it->refs);
    return S_OK;
}

static ULONG item_release(item_t *it) {
    LONG n = InterlockedDecrement(&it->refs);
    if (n == 0) {
        HeapFree(GetProcessHeap(), 0, it);
        InterlockedDecrement(&live);
    }
    return (ULONG)n;
}

static HRESULT STDMETHODCALLTYPE cmd_query(IExplorerCommand *p, REFIID riid, void **out) { return item_query(from_cmd(p), riid, out); }
static ULONG STDMETHODCALLTYPE cmd_addref(IExplorerCommand *p) { return (ULONG)InterlockedIncrement(&from_cmd(p)->refs); }
static ULONG STDMETHODCALLTYPE cmd_release(IExplorerCommand *p) { return item_release(from_cmd(p)); }

static HRESULT STDMETHODCALLTYPE cmd_title(IExplorerCommand *p, IShellItemArray *items, LPWSTR *out) {
    (void)items;
    if (out == NULL) return E_POINTER;
    *out = NULL;
    HKEY k;
    if (!open_item(from_cmd(p)->path, &k)) return E_FAIL;
    wchar_t text[TEXT_CAP];
    bool ok = item_text(k, text, TEXT_CAP);
    RegCloseKey(k);
    return ok ? SHStrDupW(text, out) : E_FAIL;
}

static HRESULT STDMETHODCALLTYPE cmd_icon(IExplorerCommand *p, IShellItemArray *items, LPWSTR *out) {
    (void)items;
    if (out == NULL) return E_POINTER;
    *out = NULL;
    HKEY k;
    if (!open_item(from_cmd(p)->path, &k)) return E_FAIL;
    wchar_t *mine = HeapAlloc(GetProcessHeap(), 0, 2 * PATH_CAP * sizeof *mine);
    HRESULT hr = E_NOTIMPL;
    if (mine && value(k, L"Icon", mine, PATH_CAP) && mine[0] && resolve(mine, mine + PATH_CAP, PATH_CAP)) hr = SHStrDupW(mine + PATH_CAP, out);
    if (mine) HeapFree(GetProcessHeap(), 0, mine);
    RegCloseKey(k);
    return hr;
}

static HRESULT STDMETHODCALLTYPE cmd_tip(IExplorerCommand *p, IShellItemArray *items, LPWSTR *out) {
    (void)p;
    (void)items;
    if (out) *out = NULL;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE cmd_name(IExplorerCommand *p, GUID *out) {
    (void)p;
    if (out) *out = GUID_NULL;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE cmd_state(IExplorerCommand *p, IShellItemArray *items, BOOL slow, EXPCMDSTATE *out) {
    (void)slow;
    if (out == NULL) return E_POINTER;
    *out = ECS_ENABLED;
    HKEY k;
    if (!open_item(from_cmd(p)->path, &k)) {
        *out = ECS_HIDDEN;
        return S_OK;
    }
    wchar_t multi[16];
    DWORD count = 0;
    if (value(k, L"Multi", multi, 16) && wcscmp(multi, L"single") == 0 && items && SUCCEEDED(IShellItemArray_GetCount(items, &count)) && count > 1)
        *out = ECS_HIDDEN;
    RegCloseKey(k);
    return S_OK;
}

// Appends s to a growing command line; false when it does not fit.
static bool put(wchar_t *line, size_t *len, const wchar_t *s, size_t n) {
    if (*len + n + 1 > PATH_CAP) return false;
    memcpy(line + *len, s, n * sizeof *s);
    *len += n;
    line[*len] = 0;
    return true;
}

// A path as one argument for CommandLineToArgvW's rules: quoted, a trailing backslash doubled.
static bool put_quoted(wchar_t *line, size_t *len, const wchar_t *path) {
    size_t n = wcslen(path);
    if (wcschr(path, L'"')) return false;               // not a file system path
    return put(line, len, L"\"", 1) && put(line, len, path, n) && (n == 0 || path[n - 1] != L'\\' || put(line, len, L"\\", 1)) &&
           put(line, len, L"\"", 1);
}

// Starts the program once with paths[0..count): args with %1 (the first) and %* (all of them).
static void run(const wchar_t *program, const wchar_t *args, wchar_t **paths, DWORD count) {
    wchar_t *line = HeapAlloc(GetProcessHeap(), 0, PATH_CAP * sizeof *line);
    if (line == NULL) return;
    size_t len = 0;
    bool ok = put_quoted(line, &len, program), used = false;
    if (ok && args[0]) ok = put(line, &len, L" ", 1);
    for (const wchar_t *a = args; ok && *a; ++a) {
        if (a[0] == L'%' && (a[1] == L'1' || a[1] == L'V' || a[1] == L'v')) {
            // As written; before a closing quote a path's last backslash is doubled ("C:\" would
            // otherwise take the quote with it).
            size_t pl = count ? wcslen(paths[0]) : 0;
            ok = count == 0 || (!wcschr(paths[0], L'"') && put(line, &len, paths[0], pl) &&
                                (pl == 0 || paths[0][pl - 1] != L'\\' || a[2] != L'"' || put(line, &len, L"\\", 1)));
            used = true;
            ++a;
        } else if (a[0] == L'%' && a[1] == L'*') {
            for (DWORD i = 0; ok && i < count; ++i) ok = (i == 0 || put(line, &len, L" ", 1)) && put_quoted(line, &len, paths[i]);
            used = true;
            ++a;
        } else {
            ok = put(line, &len, a, 1);
        }
    }
    for (DWORD i = 0; ok && !used && i < count; ++i) ok = put(line, &len, L" ", 1) && put_quoted(line, &len, paths[i]);
    if (ok) {
        // In the selected item's folder, or the folder itself.
        wchar_t *dir = NULL;
        if (count && (dir = HeapAlloc(GetProcessHeap(), 0, PATH_CAP * sizeof *dir)) != NULL) {
            wcsncpy(dir, paths[0], PATH_CAP - 1);
            dir[PATH_CAP - 1] = 0;
            DWORD a = GetFileAttributesW(dir);
            if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) {
                wchar_t *slash = wcsrchr(dir, L'\\');
                if (slash && slash > dir + 2) *slash = 0;
                else if (slash) slash[1] = 0;
            }
        }
        STARTUPINFOW si;
        PROCESS_INFORMATION pi;
        memset(&si, 0, sizeof si);
        si.cb = sizeof si;
        if (CreateProcessW(program, line, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            say(L"started: ", line, 0);
        } else {
            say(L"not started: ", line, GetLastError());
        }
        if (dir) HeapFree(GetProcessHeap(), 0, dir);
    }
    HeapFree(GetProcessHeap(), 0, line);
}

static HRESULT STDMETHODCALLTYPE cmd_invoke(IExplorerCommand *p, IShellItemArray *items, IBindCtx *ctx) {
    (void)ctx;
    HKEY k;
    say(L"chosen: ", from_cmd(p)->path, 0);
    if (!open_item(from_cmd(p)->path, &k)) return E_FAIL;
    wchar_t *buf = HeapAlloc(GetProcessHeap(), 0, 3 * PATH_CAP * sizeof *buf);
    wchar_t multi[16] = L"each";
    HRESULT hr = E_FAIL;
    if (buf && value(k, L"Command", buf, PATH_CAP) && buf[0] && resolve(buf, buf + PATH_CAP, PATH_CAP)) {
        wchar_t *program = buf + PATH_CAP, *args = buf + 2 * PATH_CAP;
        if (!value(k, L"Args", args, PATH_CAP)) args[0] = 0;
        (void)value(k, L"Multi", multi, 16);
        // The selection as file system paths (what is not in the file system is left out).
        wchar_t *paths[MAX_RUNS];
        DWORD count = 0, n = 0;
        if (items) (void)IShellItemArray_GetCount(items, &count);
        for (DWORD i = 0; i < count && n < MAX_RUNS; ++i) {
            IShellItem *si = NULL;
            if (FAILED(IShellItemArray_GetItemAt(items, i, &si))) continue;
            wchar_t *path = NULL;
            if (SUCCEEDED(IShellItem_GetDisplayName(si, SIGDN_FILESYSPATH, &path)) && path) paths[n++] = path;
            IShellItem_Release(si);
        }
        if (wcscmp(multi, L"each") == 0 && n > 1) {
            for (DWORD i = 0; i < n; ++i) run(program, args, &paths[i], 1);
        } else {
            run(program, args, paths, n);
        }
        for (DWORD i = 0; i < n; ++i) CoTaskMemFree(paths[i]);
        hr = S_OK;
    } else {
        say(L"no usable Command for ", from_cmd(p)->path, GetLastError());
    }
    if (buf) HeapFree(GetProcessHeap(), 0, buf);
    RegCloseKey(k);
    return hr;
}

static HRESULT STDMETHODCALLTYPE cmd_flags(IExplorerCommand *p, EXPCMDFLAGS *out) {
    if (out == NULL) return E_POINTER;
    *out = from_cmd(p)->nsubs ? ECF_HASSUBCOMMANDS : ECF_DEFAULT;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE cmd_subs(IExplorerCommand *p, IEnumExplorerCommand **out) {
    if (out == NULL) return E_POINTER;
    *out = NULL;
    item_t *it = from_cmd(p);
    if (it->nsubs == 0) return E_NOTIMPL;
    item_t *e = item_new(it->path);         // an enumerator of its own, at the start
    if (e == NULL) return E_OUTOFMEMORY;
    *out = &e->en;
    return S_OK;
}

static const IExplorerCommandVtbl cmd_vtbl = { cmd_query, cmd_addref, cmd_release, cmd_title, cmd_icon, cmd_tip,
                                               cmd_name,  cmd_state,  cmd_invoke,  cmd_flags, cmd_subs };

static HRESULT STDMETHODCALLTYPE en_query(IEnumExplorerCommand *p, REFIID riid, void **out) { return item_query(from_en(p), riid, out); }
static ULONG STDMETHODCALLTYPE en_addref(IEnumExplorerCommand *p) { return (ULONG)InterlockedIncrement(&from_en(p)->refs); }
static ULONG STDMETHODCALLTYPE en_release(IEnumExplorerCommand *p) { return item_release(from_en(p)); }

static HRESULT STDMETHODCALLTYPE en_next(IEnumExplorerCommand *p, ULONG want, IExplorerCommand **out, ULONG *got) {
    item_t *it = from_en(p);
    ULONG n = 0;
    if (out == NULL) return E_POINTER;
    while (n < want && it->at < it->nsubs) {
        wchar_t path[400];
        const wchar_t *sub = it->subs[it->at++];
        if (wcslen(it->path) + wcslen(sub) + 2 > 400) continue;
        wcscpy(path, it->path);
        wcscat(path, L"\\");
        wcscat(path, sub);
        item_t *child = item_new(path);
        if (child == NULL) continue;
        out[n++] = &child->cmd;
    }
    if (got) *got = n;
    return n == want ? S_OK : S_FALSE;
}

static HRESULT STDMETHODCALLTYPE en_skip(IEnumExplorerCommand *p, ULONG n) {
    item_t *it = from_en(p);
    it->at = it->at + n > it->nsubs ? it->nsubs : it->at + n;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE en_reset(IEnumExplorerCommand *p) {
    from_en(p)->at = 0;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE en_clone(IEnumExplorerCommand *p, IEnumExplorerCommand **out) {
    if (out == NULL) return E_POINTER;
    item_t *e = item_new(from_en(p)->path);
    *out = e ? &e->en : NULL;
    if (e) e->at = from_en(p)->at;
    return e ? S_OK : E_OUTOFMEMORY;
}

static const IEnumExplorerCommandVtbl en_vtbl = { en_query, en_addref, en_release, en_next, en_skip, en_reset, en_clone };

// The item at `path` (one reference), or NULL when the registry has none.
static item_t *item_new(const wchar_t *path) {
    HKEY k;
    if (wcslen(path) >= 400 || !open_item(path, &k)) return NULL;
    item_t *it = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *it);
    if (it) {
        it->cmd.lpVtbl = (IExplorerCommandVtbl *)&cmd_vtbl;
        it->en.lpVtbl = (IEnumExplorerCommandVtbl *)&en_vtbl;
        it->refs = 1;
        wcscpy(it->path, path);
        // Items: the names of the sub keys, in order.
        DWORD type = 0, size = 0;
        if (RegQueryValueExW(k, L"Items", NULL, &type, NULL, &size) == ERROR_SUCCESS && type == REG_MULTI_SZ && size >= 2 * sizeof(wchar_t) &&
            size <= MAX_ITEMS * 64 * sizeof(wchar_t)) {
            wchar_t *list = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size + 2 * sizeof(wchar_t));
            if (list && RegQueryValueExW(k, L"Items", NULL, &type, (BYTE *)list, &size) == ERROR_SUCCESS) {
                for (const wchar_t *s = list; *s && it->nsubs < MAX_ITEMS; s += wcslen(s) + 1) {
                    if (wcslen(s) < 64 && !wcschr(s, L'\\')) wcscpy(it->subs[it->nsubs++], s);
                }
            }
            if (list) HeapFree(GetProcessHeap(), 0, list);
        }
        InterlockedIncrement(&live);
    }
    RegCloseKey(k);
    return it;
}

// ---- the class factory -------------------------------------------------------------------------

typedef struct {
    IClassFactory f;
    LONG          refs;
    wchar_t       clsid[40];
} factory_t;

static HRESULT STDMETHODCALLTYPE f_query(IClassFactory *p, REFIID riid, void **out) {
    if (out == NULL) return E_POINTER;
    *out = NULL;
    if (!IsEqualIID(riid, &IID_IUnknown) && !IsEqualIID(riid, &IID_IClassFactory)) return E_NOINTERFACE;
    *out = p;
    InterlockedIncrement(&((factory_t *)p)->refs);
    return S_OK;
}

static ULONG STDMETHODCALLTYPE f_addref(IClassFactory *p) { return (ULONG)InterlockedIncrement(&((factory_t *)p)->refs); }

static ULONG STDMETHODCALLTYPE f_release(IClassFactory *p) {
    LONG n = InterlockedDecrement(&((factory_t *)p)->refs);
    if (n == 0) {
        HeapFree(GetProcessHeap(), 0, p);
        InterlockedDecrement(&live);
    }
    return (ULONG)n;
}

static HRESULT STDMETHODCALLTYPE f_create(IClassFactory *p, IUnknown *outer, REFIID riid, void **out) {
    if (out == NULL) return E_POINTER;
    *out = NULL;
    if (outer) return CLASS_E_NOAGGREGATION;
    item_t *it = item_new(((factory_t *)p)->clsid);
    if (it == NULL) return E_FAIL;
    HRESULT hr = item_query(it, riid, out);
    item_release(it);
    return hr;
}

static HRESULT STDMETHODCALLTYPE f_lock(IClassFactory *p, BOOL lock) {
    (void)p;
    if (lock) InterlockedIncrement(&live);
    else InterlockedDecrement(&live);
    return S_OK;
}

static const IClassFactoryVtbl f_vtbl = { f_query, f_addref, f_release, f_create, f_lock };

// Any class the registry has an item for.
STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void **out) {
    if (out == NULL) return E_POINTER;
    *out = NULL;
    wchar_t text[40];
    HKEY k;
    if (StringFromGUID2(clsid, text, 40) == 0 || !open_item(text, &k)) return CLASS_E_CLASSNOTAVAILABLE;
    RegCloseKey(k);
    factory_t *f = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *f);
    if (f == NULL) return E_OUTOFMEMORY;
    f->f.lpVtbl = (IClassFactoryVtbl *)&f_vtbl;
    f->refs = 1;
    wcscpy(f->clsid, text);
    InterlockedIncrement(&live);
    HRESULT hr = f_query(&f->f, riid, out);
    f_release(&f->f);
    return hr;
}

STDAPI DllCanUnloadNow(void) { return live == 0 ? S_OK : S_FALSE; }

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        self = inst;
        DisableThreadLibraryCalls(inst);
    }
    return TRUE;
}
