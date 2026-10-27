// src/setup/rubrapack_setup.c - the chain bootstrapper (setup.exe; include/rubrapack/chain.h).
//
// `rubrapack build` appends packages and a manifest to a copy of this program. At run time it
// finds that payload at the end of its own file (before an Authenticode signature, if any),
// checks every package's SHA-256, writes the packages to a new temporary folder and installs them
// in order: an MSI with msi.dll, a patch with MsiApplyPatch, an MSIX package or bundle with Windows'
// PackageManager (RFC-0031 B14; for every user - staged and provisioned - when the chain is
// elevated, else for the user). A package that is already installed is skipped; a vital package
// that fails stops the chain (each package rolls itself back). /uninstall removes the packages in
// reverse order, /repair repairs the installed MSI packages (a patch is repaired with its product,
// an MSIX has nothing to repair). Started with every package installed and neither
// switch, it asks which of the two to do (nothing at /quiet or /passive).
// Built by `nob parts` for x64, x86 and arm64, reproducibly; Windows only.
//
//   setup.exe [/quiet | /passive] [/uninstall | /repair] [/log <file>] [/lang <code>]
//   /lang: the setup program's texts in that language ("ko", "ar", ...), not the display language's
//
// Exit code: 0, 3010 when a package asks for a restart, or the failing package's Windows Installer
// error; 1620 for a damaged payload, 1602 when elevation is refused.

#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>
#include <bcrypt.h>
#include <msi.h>
#include <sddl.h>
#include <shellapi.h>

#include <stdint.h>
#include <string.h>

#include "../ca/rp_appx.h"
#include "../ca/rp_ask.h"

enum {
    TRAILER = 32,
    MAX_PACKAGES = 64,
    FLAG_VITAL = 1,
    FLAG_MSIX = 2,
    FLAG_MSP = 4,
    FLAG_BUNDLE = 8,
    CHAIN_ELEVATE = 1,
    EXIT_DAMAGED = 1620,
    EXIT_CANCELLED = 1602,
};

typedef struct {
    const char    *id, *properties, *product_code, *version;
    uint32_t       id_len, properties_len, product_code_len, version_len, flags;
    uint64_t       offset, size;
    const uint8_t *sha256;
} package_t;

typedef struct {
    const char *name;
    uint32_t    name_len, flags, count;
    uint32_t    texts_at;           // where the manifest goes on after the packages (the texts block, if any)
    package_t   p[MAX_PACKAGES];
} chain_t;

// The program's own texts: English here; another language from the manifest's texts block, which
// `rubrapack build` fills from language packs ("Setup.*"), picked by the user's display language.
// {1} is a package's ID, {2} an error number.
enum { T_DAMAGED, T_ASK, T_REPAIR, T_REMOVE, T_CANCEL, T_REMOVEASK, T_REMOVED, T_REMOVEFAILED, T_TEMPFAILED, T_REPAIRFAILED,
       T_INSTALLFAILED, T_REPAIRED, T_FINISHED, T_RESTART, T_COUNT };
static const wchar_t *texts[T_COUNT] = {
    L"This setup program is damaged: download it again.",
    L"These programs are already installed. Choose what to do.",
    L"Re&pair",
    L"&Remove",
    L"Cancel",
    L"Remove these programs from this computer?",
    L"The programs were removed.",
    L"A program could not be removed.",
    L"Setup could not create a temporary folder.",
    L"{1} could not be repaired (error {2}).",
    L"{1} could not be installed (error {2}). Nothing after it was installed.",
    L"The programs were repaired.",
    L"Setup finished.",
    L"Restart Windows to complete the changes.",
};

// `tmpl` with {1} as `one` and {2} as the number `two`.
static void fill(wchar_t *out, size_t cap, const wchar_t *tmpl, const wchar_t *one, unsigned two) {
    wchar_t num[16];
    wsprintfW(num, two > 0xFFFF ? L"0x%08X" : L"%u", two);     // an HRESULT (an MSIX) in hex
    size_t o = 0;
    for (const wchar_t *p = tmpl; *p && o + 1 < cap; ++p) {
        const wchar_t *ins = NULL;
        if (p[0] == L'{' && (p[1] == L'1' || p[1] == L'2') && p[2] == L'}') {
            ins = p[1] == L'1' ? one : num;
            p += 2;
        }
        if (ins == NULL) { out[o++] = *p; continue; }
        for (; *ins && o + 1 < cap; ++ins) out[o++] = *ins;
    }
    out[o] = 0;
}

static bool quiet, passive;
static bool rtl;            // the texts are in a language that reads right to left (texts_load)

static void say(const wchar_t *title, const wchar_t *text, bool error) {
    if (quiet || (passive && !error)) return;
    MessageBoxW(NULL, text, title, MB_OK | (error ? MB_ICONERROR : MB_ICONINFORMATION) | rp_mb_rtl(rtl));
}

// The closing message: with "restart Windows" after it when a package asked for a restart (3010).
static void say_done(const wchar_t *title, const wchar_t *text, int result) {
    wchar_t both[1024];
    if (result == ERROR_SUCCESS_REBOOT_REQUIRED && lstrlenW(text) + lstrlenW(texts[T_RESTART]) + 3 < (int)(sizeof both / sizeof both[0])) {
        wsprintfW(both, L"%s\n\n%s", text, texts[T_RESTART]);
        text = both;
    }
    say(title, text, false);
}

// UTF-8 to a new wide string (LocalFree), or NULL.
static wchar_t *wide(const char *s, uint32_t n) {
    int w = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, (int)n, NULL, 0);
    if (n && w <= 0) return NULL;
    wchar_t *out = LocalAlloc(LMEM_FIXED, ((size_t)w + 1) * sizeof *out);
    if (out == NULL) return NULL;
    if (n) MultiByteToWideChar(CP_UTF8, 0, s, (int)n, out, w);
    out[w] = 0;
    return out;
}

static bool read_at(HANDLE f, uint64_t at, void *buf, uint32_t n) {
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)at;
    DWORD got = 0;
    return SetFilePointerEx(f, li, NULL, FILE_BEGIN) && ReadFile(f, buf, n, &got, NULL) && got == n;
}

static uint32_t u32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t u64(const uint8_t *p) { return u32(p) | (uint64_t)u32(p + 4) << 32; }

// Where the payload ends: before the certificate table when the file is signed, else the file's end.
static bool payload_end(HANDLE f, uint64_t size, uint64_t *end) {
    uint8_t h[512];
    *end = size;
    if (size < sizeof h || !read_at(f, 0, h, sizeof h) || h[0] != 'M' || h[1] != 'Z') return false;
    uint32_t pe = u32(h + 0x3C);
    uint8_t n[256];
    if (!read_at(f, pe, n, sizeof n) || memcmp(n, "PE\0\0", 4) != 0) return false;
    uint16_t magic = (uint16_t)(n[24] | n[25] << 8);
    uint32_t dirs = magic == 0x20B ? 24 + 112 : 24 + 96;        // optional header: data directories
    uint32_t cert = u32(n + dirs + 4 * 8), cert_size = u32(n + dirs + 4 * 8 + 4);
    if (cert && cert_size && cert <= size) *end = cert;
    return true;
}

static bool parse(const uint8_t *m, uint32_t n, uint64_t payload_len, chain_t *c) {
    uint32_t at = 0;
#define NEED(k) do { if ((k) > n - at) return false; } while (0)
#define STR(s, l) do { NEED(4); l = u32(m + at); at += 4; NEED(l); s = (const char *)m + at; at += l; } while (0)
    NEED(16);
    if (memcmp(m, "RPCHAIN1", 8) != 0) return false;
    c->count = u32(m + 8);
    c->flags = u32(m + 12);
    at = 16;
    if (c->count == 0 || c->count > MAX_PACKAGES) return false;
    const char *skip;
    uint32_t skip_len;
    STR(c->name, c->name_len);
    STR(skip, skip_len);            // manufacturer
    STR(skip, skip_len);            // version
    for (uint32_t i = 0; i < c->count; ++i) {
        package_t *p = &c->p[i];
        STR(p->id, p->id_len);
        // The id names the package's temporary file: a short plain name, as `rubrapack build` writes.
        if (p->id_len == 0 || p->id_len > 60) return false;
        for (uint32_t k = 0; k < p->id_len; ++k) {
            char ch = p->id[k];
            if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_')) return false;
        }
        STR(p->properties, p->properties_len);
        STR(p->product_code, p->product_code_len);
        STR(p->version, p->version_len);
        NEED(4 + 16 + 32);
        p->flags = u32(m + at);
        p->offset = u64(m + at + 4);
        p->size = u64(m + at + 12);
        p->sha256 = m + at + 20;
        at += 52;
        if (p->offset > payload_len || p->size > payload_len - p->offset) return false;
    }
    (void)skip;
    (void)skip_len;
    c->texts_at = at;
    return true;
#undef STR
#undef NEED
}

// The texts block: "RPTEXTS1", the number of languages; each its code, the number of its LANGIDs
// and those, then T_COUNT texts (an empty one stays English). Anything out of place: English.
// `want`: a language code (/lang) chosen instead of the user's display language.
static void texts_load(const uint8_t *m, uint32_t n, uint32_t at, const wchar_t *want) {
    if (at > n || n - at < 12 || memcmp(m + at, "RPTEXTS1", 8) != 0) return;
    uint32_t langs = u32(m + at + 8);
    at += 12;
    LANGID ui = GetUserDefaultUILanguage();
    for (uint32_t i = 0; i < langs && i < 64; ++i) {
        if (n - at < 4) return;
        uint32_t cl = u32(m + at);
        if (cl > n - at - 4) return;
        bool named = false;         // the code is the one /lang gave
        if (want && cl < 16) {
            named = true;
            for (uint32_t k = 0; named && k < 16; ++k) {
                wchar_t a = k < cl ? (wchar_t)m[at + 4 + k] : 0, b = want[k];
                if (a == L'-') a = L'_';
                if (b == L'-') b = L'_';
                named = (a | 0x20) == (b | 0x20);
                if (a == 0 || b == 0) break;
            }
        }
        at += 4 + cl;
        if (n - at < 4) return;
        uint32_t ids = u32(m + at);
        at += 4;
        if (ids > 64 || ids * 4 > n - at) return;
        bool mine = want ? named : false;
        LANGID first = ids ? (LANGID)u32(m + at) : 0;
        for (uint32_t k = 0; !want && k < ids; ++k) mine = mine || u32(m + at + 4 * k) == ui;
        at += ids * 4;
        for (int t = 0; t < T_COUNT; ++t) {
            if (n - at < 4) return;
            uint32_t l = u32(m + at);
            if (l > n - at - 4) return;
            if (mine && l) {
                wchar_t *w = wide((const char *)m + at + 4, l);
                if (w) texts[t] = w;
            }
            at += 4 + l;
        }
        if (mine) {
            rtl = rp_lcid_rtl(want ? first : ui);
            return;
        }
    }
}

// Copies bytes [at, at + size) of f to a new file `path`, checking their SHA-256.
static bool extract(HANDLE f, uint64_t at, uint64_t size, const uint8_t want[32], const wchar_t *path) {
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE h = NULL;
    HANDLE out = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    bool ok = out != INVALID_HANDLE_VALUE && BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) == 0 &&
              BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0) == 0;
    static uint8_t buf[1 << 16];
    for (uint64_t done = 0; ok && done < size;) {
        uint32_t n = size - done < sizeof buf ? (uint32_t)(size - done) : (uint32_t)sizeof buf;
        DWORD wrote = 0;
        ok = read_at(f, at + done, buf, n) && BCryptHashData(h, buf, n, 0) == 0 && WriteFile(out, buf, n, &wrote, NULL) && wrote == n;
        done += n;
    }
    uint8_t got[32];
    if (ok) ok = BCryptFinishHash(h, got, 32, 0) == 0 && memcmp(got, want, 32) == 0;
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    if (!ok) DeleteFileW(path);
    return ok;
}

static bool elevated(void) {
    HANDLE t;
    TOKEN_ELEVATION e = { 0 };
    DWORD n = 0;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t)) return false;
    bool ok = GetTokenInformation(t, TokenElevation, &e, sizeof e, &n) && e.TokenIsElevated;
    CloseHandle(t);
    return ok;
}

// Runs this program again elevated with the same arguments and returns its exit code.
static int relaunch(const wchar_t *self, const wchar_t *args) {
    SHELLEXECUTEINFOW sei = { .cbSize = sizeof sei, .fMask = SEE_MASK_NOCLOSEPROCESS, .lpVerb = L"runas",
                              .lpFile = self, .lpParameters = args, .nShow = SW_SHOWNORMAL };
    if (!ShellExecuteExW(&sei) || sei.hProcess == NULL) return EXIT_CANCELLED;
    WaitForSingleObject(sei.hProcess, INFINITE);
    DWORD code = EXIT_CANCELLED;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    return (int)code;
}

typedef LONG(WINAPI *by_family_fn)(PCWSTR, UINT32 *, PWSTR *, UINT32 *, WCHAR *);

// The full names of the user's packages of an MSIX family (one LocalAlloc block, NULL-terminated), or NULL.
static PWSTR *family_packages(const wchar_t *family) {
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    by_family_fn by_family = k32 ? (by_family_fn)(void (*)(void))GetProcAddress(k32, "GetPackagesByPackageFamily") : NULL;
    UINT32 count = 0, chars = 0;
    if (by_family == NULL || by_family(family, &count, NULL, &chars, NULL) != ERROR_INSUFFICIENT_BUFFER || count == 0) return NULL;
    PWSTR *names = LocalAlloc(LMEM_FIXED | LMEM_ZEROINIT, ((size_t)count + 1) * sizeof *names + ((size_t)chars + 1) * sizeof(WCHAR));
    if (names && by_family(family, &count, names, &chars, (WCHAR *)(names + count + 1)) != ERROR_SUCCESS) {
        LocalFree(names);
        names = NULL;
    }
    if (names) names[count] = NULL;
    return names;
}

// Whether a patch is applied to `product` (in any context this program can see).
static bool patch_applied(const wchar_t *patch, const wchar_t *product) {
    static const MSIINSTALLCONTEXT contexts[] = { MSIINSTALLCONTEXT_MACHINE, MSIINSTALLCONTEXT_USERUNMANAGED, MSIINSTALLCONTEXT_USERMANAGED };
    for (size_t i = 0; i < sizeof contexts / sizeof contexts[0]; ++i) {
        wchar_t state[8];
        DWORD n = sizeof state / sizeof state[0];
        if (MsiGetPatchInfoExW(patch, product, NULL, contexts[i], INSTALLPROPERTY_PATCHSTATE, state, &n) == ERROR_SUCCESS && lstrcmpW(state, L"1") == 0) {
            return true;
        }
    }
    return false;
}

// The i-th product code of a patch's targets ("{...};{...}"), into `out`; false past the last.
static bool target(const package_t *p, uint32_t i, wchar_t out[39]) {
    uint32_t at = 0;
    for (uint32_t k = 0; k < i; ++k) {
        while (at < p->version_len && p->version[at] != ';') ++at;
        if (at == p->version_len) return false;
        ++at;
    }
    if (p->version_len - at < 38) return false;
    for (int k = 0; k < 38; ++k) out[k] = (wchar_t)(unsigned char)p->version[at + k];
    out[38] = 0;
    return true;
}

static bool installed(const package_t *p) {
    wchar_t *pc = wide(p->product_code, p->product_code_len);
    bool yes = false;
    if (pc && (p->flags & FLAG_MSIX)) {
        PWSTR *names = family_packages(pc);
        yes = names != NULL;
        LocalFree(names);
    } else if (pc && (p->flags & FLAG_MSP)) {
        wchar_t product[39];
        for (uint32_t i = 0; !yes && target(p, i, product); ++i) yes = patch_applied(pc, product);
    } else if (pc) {
        yes = MsiQueryProductStateW(pc) == INSTALLSTATE_DEFAULT;
    }
    LocalFree(pc);
    return yes;
}

static const wchar_t *ext(const package_t *p) {
    return p->flags & FLAG_MSP ? L"msp" : p->flags & FLAG_BUNDLE ? L"msixbundle" : p->flags & FLAG_MSIX ? L"msix" : L"msi";
}

// An MSIX package or bundle added (for every user when `machine`) or removed (every package of its
// family the user has); a Windows Installer error number for the chain's exit code, *hr the cause.
static UINT appx(const package_t *p, const wchar_t *path, bool machine, bool remove, HRESULT *hr) {
    wchar_t *family = wide(p->product_code, p->product_code_len);
    *hr = E_OUTOFMEMORY;
    if (family == NULL) return ERROR_OUTOFMEMORY;
    if (remove) {
        PWSTR *names = family_packages(family);
        *hr = S_OK;
        for (size_t i = 0; names && names[i] && SUCCEEDED(*hr); ++i) {
            appx_job_t j = { .full_name = names[i], .family = family, .machine = machine, .remove = true };
            *hr = appx_run(&j);
        }
        LocalFree(names);
    } else {
        appx_job_t j = { .package = path, .family = family, .machine = machine, .allow_unsigned = true };
        *hr = appx_run(&j);
    }
    LocalFree(family);
    return SUCCEEDED(*hr) ? ERROR_SUCCESS : ERROR_INSTALL_FAILURE;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmdline, int show) {
    (void)inst;
    (void)prev;
    (void)show;
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool uninstall = false, repair = false;
    const wchar_t *log = NULL, *lang = NULL;
    for (int i = 1; argv && i < argc; ++i) {
        if (lstrcmpiW(argv[i], L"/quiet") == 0 || lstrcmpiW(argv[i], L"/q") == 0) quiet = true;
        else if (lstrcmpiW(argv[i], L"/passive") == 0) passive = true;
        else if (lstrcmpiW(argv[i], L"/uninstall") == 0 || lstrcmpiW(argv[i], L"/x") == 0) uninstall = true;
        else if (lstrcmpiW(argv[i], L"/repair") == 0) repair = true;
        else if (lstrcmpiW(argv[i], L"/log") == 0 && i + 1 < argc) log = argv[++i];
        else if (lstrcmpiW(argv[i], L"/lang") == 0 && i + 1 < argc) lang = argv[++i];
        else {
            say(L"Setup", L"Usage: setup.exe [/quiet | /passive] [/uninstall | /repair] [/log <file>] [/lang <code>]", true);
            return ERROR_INVALID_PARAMETER;
        }
    }

    // The payload: packages, then the manifest, then a 32-byte trailer ending "RPCHAIN!".
    wchar_t self[MAX_PATH * 4];
    DWORD sn = GetModuleFileNameW(NULL, self, (DWORD)(sizeof self / sizeof self[0]));
    HANDLE f = sn ? CreateFileW(self, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL) : INVALID_HANDLE_VALUE;
    LARGE_INTEGER size = { 0 };
    uint64_t end = 0;
    uint8_t t[TRAILER];
    chain_t c;
    uint8_t *manifest = NULL;
    bool ok = f != INVALID_HANDLE_VALUE && GetFileSizeEx(f, &size) && payload_end(f, (uint64_t)size.QuadPart, &end) &&
              end >= TRAILER && read_at(f, end - TRAILER, t, TRAILER) && memcmp(t + 24, "RPCHAIN!", 8) == 0;
    uint64_t start = ok ? u64(t) : 0, mat = ok ? u64(t + 8) : 0;
    uint32_t mlen = ok ? u32(t + 16) : 0;
    ok = ok && start <= mat && mat <= end - TRAILER && mlen <= end - TRAILER - mat && mlen > 0 && (manifest = LocalAlloc(LMEM_FIXED, mlen)) != NULL &&
         read_at(f, mat, manifest, mlen) && parse(manifest, mlen, mat - start, &c);
    if (!ok) {
        say(L"Setup", texts[T_DAMAGED], true);
        return EXIT_DAMAGED;
    }
    texts_load(manifest, mlen, c.texts_at, lang);
    wchar_t *title = wide(c.name, c.name_len);
    if (title == NULL) return EXIT_DAMAGED;

    // Everything is installed already and nothing was said: ask what to do (before any elevation,
    // which then carries the answer).
    bool all = c.count > 0;
    for (uint32_t k = 0; k < c.count; ++k) all = all && installed(&c.p[k]);
    const wchar_t *extra = L"";
    if (all && !uninstall && !repair && !quiet && !passive) {
        rp_ask_t a = { title, texts[T_ASK], texts[T_REPAIR], texts[T_REMOVE], texts[T_CANCEL], rtl };
        int r;
        for (;;) {      // Remove is asked once more (No is the default button, and leads back)
            r = rp_ask(NULL, &a);
            if (r != RP_ASK_SECOND || MessageBoxW(NULL, texts[T_REMOVEASK], title,
                                                  MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2 | MB_TOPMOST | MB_SETFOREGROUND | rp_mb_rtl(rtl)) == IDYES) break;
        }
        if (r == RP_ASK_CANCEL) {
            CloseHandle(f);
            return EXIT_CANCELLED;
        }
        repair = r == RP_ASK_FIRST;
        uninstall = r == RP_ASK_SECOND;
        extra = repair ? L" /repair" : L" /uninstall";
    }

    if ((c.flags & CHAIN_ELEVATE) && !elevated()) {
        size_t an = (size_t)lstrlenW(cmdline) + 16;
        wchar_t *args = LocalAlloc(LMEM_FIXED, an * sizeof *args);
        CloseHandle(f);
        if (args == NULL) return ERROR_OUTOFMEMORY;
        lstrcpyW(args, cmdline);
        lstrcatW(args, extra);
        int code = relaunch(self, args);
        LocalFree(args);
        return code;
    }

    if (quiet) MsiSetInternalUI(INSTALLUILEVEL_NONE, NULL);
    else MsiSetInternalUI(passive ? INSTALLUILEVEL_BASIC | INSTALLUILEVEL_PROGRESSONLY : INSTALLUILEVEL_BASIC, NULL);
    if (log) MsiEnableLogW(INSTALLLOGMODE_VERBOSE, log, INSTALLLOGATTRIBUTES_APPEND);

    int result = 0;
    wchar_t msg[512];
    if (uninstall) {
        for (uint32_t k = c.count; k-- > 0;) {
            const package_t *p = &c.p[k];
            if (!installed(p)) continue;
            wchar_t *pc = wide(p->product_code, p->product_code_len);
            UINT r = ERROR_OUTOFMEMORY;
            if (pc && (p->flags & FLAG_MSIX)) {
                HRESULT hr;
                r = appx(p, NULL, (c.flags & CHAIN_ELEVATE) != 0, true, &hr);
            } else if (pc && (p->flags & FLAG_MSP)) {
                // From every target it is applied to (a product that goes later takes it along anyway).
                wchar_t product[39];
                r = ERROR_SUCCESS;
                for (uint32_t i = 0; target(p, i, product); ++i) {
                    if (!patch_applied(pc, product)) continue;
                    UINT one = MsiRemovePatchesW(pc, product, INSTALLTYPE_SINGLE_INSTANCE, NULL);     // the only type it takes
                    if (one != ERROR_SUCCESS && r != ERROR_SUCCESS_REBOOT_REQUIRED) r = one;
                }
            } else if (pc) {
                r = MsiConfigureProductW(pc, INSTALLLEVEL_DEFAULT, INSTALLSTATE_ABSENT);
            }
            LocalFree(pc);
            if (r == ERROR_SUCCESS_REBOOT_REQUIRED) result = (int)r;
            else if (r != ERROR_SUCCESS && result == 0) result = (int)r;
        }
        if (result == 0 || result == ERROR_SUCCESS_REBOOT_REQUIRED) say_done(title, texts[T_REMOVED], result);
        else say(title, texts[T_REMOVEFAILED], true);
        CloseHandle(f);
        return result;
    }

    // A folder of its own under %TEMP% for the packages.
    // Elevated, only SYSTEM and Administrators may write there: the same user's programs that are
    // not elevated can write in %TEMP%, and a package swapped after its check would be installed
    // with this program's rights.
    wchar_t dir[MAX_PATH], path[MAX_PATH + 80];
    DWORD dn = GetTempPathW(MAX_PATH, dir);
    bool made = false;
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, FALSE };
    bool locked = !elevated() ||
                  ConvertStringSecurityDescriptorToSecurityDescriptorW(L"O:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)", SDDL_REVISION_1, &sa.lpSecurityDescriptor, NULL);
    for (unsigned k = 0; locked && dn && dn < MAX_PATH - 40 && !made && k < 100; ++k) {
        wsprintfW(dir + dn, L"rpsetup-%08lx-%u", GetCurrentProcessId() ^ GetTickCount(), k);
        made = CreateDirectoryW(dir, sa.lpSecurityDescriptor ? &sa : NULL) != 0;
    }
    if (sa.lpSecurityDescriptor) LocalFree(sa.lpSecurityDescriptor);
    if (!made) {
        say(title, texts[T_TEMPFAILED], true);
        CloseHandle(f);
        return ERROR_CANNOT_MAKE;
    }
    for (uint32_t k = 0; k < c.count && result != EXIT_DAMAGED; ++k) {
        const package_t *p = &c.p[k];
        wchar_t *id = wide(p->id, p->id_len);
        wsprintfW(path, L"%s\\%s.%s", dir, id ? id : L"package", ext(p));
        if (id == NULL || !extract(f, start + p->offset, p->size, p->sha256, path)) {
            say(title, texts[T_DAMAGED], true);
            result = EXIT_DAMAGED;
        }
        LocalFree(id);
    }
    CloseHandle(f);
    for (uint32_t k = 0; k < c.count && (result == 0 || result == ERROR_SUCCESS_REBOOT_REQUIRED); ++k) {
        const package_t *p = &c.p[k];
        // A repair takes the installed packages only, each from its package here: the folder the
        // product was installed from is gone, and missing files need the package (error 1706 otherwise).
        // The properties are what the dialogs' Repair sets.
        if (installed(p) != repair || (repair && (p->flags & (FLAG_MSIX | FLAG_MSP)))) continue;
        wchar_t *id = wide(p->id, p->id_len), *props = repair ? wide("REINSTALL=ALL REINSTALLMODE=ecmus", 33) : wide(p->properties, p->properties_len);
        wsprintfW(path, L"%s\\%s.%s", dir, id ? id : L"package", ext(p));
        // /lang: the packages' own dialogs and messages in that language too.
        wchar_t *all = props;
        if (props && lang && lstrlenW(lang) < 16) {
            size_t pn = (size_t)lstrlenW(props) + 40;
            all = LocalAlloc(LMEM_FIXED, pn * sizeof *all);
            if (all) wsprintfW(all, L"%s RPLANGUAGE=%s", props, lang);
        }
        HRESULT hr = S_OK;
        UINT r = id == NULL || all == NULL       ? ERROR_OUTOFMEMORY
                 : (p->flags & FLAG_MSIX)        ? appx(p, path, (c.flags & CHAIN_ELEVATE) != 0, false, &hr)
                 : (p->flags & FLAG_MSP)         ? MsiApplyPatchW(path, NULL, INSTALLTYPE_DEFAULT, all)
                                                 : MsiInstallProductW(path, all);
        if (all && all != props) LocalFree(all);
        if (r == ERROR_SUCCESS_REBOOT_REQUIRED) {
            result = (int)r;
        } else if (r != ERROR_SUCCESS && (repair || (p->flags & FLAG_VITAL))) {
            fill(msg, sizeof msg / sizeof msg[0], repair ? texts[T_REPAIRFAILED] : texts[T_INSTALLFAILED], id ? id : L"?",
                 FAILED(hr) ? (unsigned)hr : (unsigned)r);
            say(title, msg, true);
            result = (int)r;
        }
        LocalFree(id);
        LocalFree(props);
    }
    for (uint32_t k = 0; k < c.count; ++k) {
        wchar_t *id = wide(c.p[k].id, c.p[k].id_len);
        wsprintfW(path, L"%s\\%s.%s", dir, id ? id : L"package", ext(&c.p[k]));
        DeleteFileW(path);
        LocalFree(id);
    }
    RemoveDirectoryW(dir);
    if (result == 0 || result == ERROR_SUCCESS_REBOOT_REQUIRED) say_done(title, repair ? texts[T_REPAIRED] : texts[T_FINISHED], result);
    LocalFree(title);
    LocalFree(manifest);
    return result;
}
