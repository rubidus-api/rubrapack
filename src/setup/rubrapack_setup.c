// src/setup/rubrapack_setup.c - the chain bootstrapper (setup.exe; include/rubrapack/chain.h).
//
// `rubrapack build` appends packages and a manifest to a copy of this program. At run time it
// finds that payload at the end of its own file (before an Authenticode signature, if any),
// checks every package's SHA-256, writes the packages to a new temporary folder and installs them
// in order with msi.dll. A package that is already installed is skipped; a vital package that fails
// stops the chain (each package rolls itself back). /uninstall removes the packages in reverse
// order. Built by `nob parts` for x64, x86 and arm64, reproducibly; Windows only.
//
//   setup.exe [/quiet | /passive] [/uninstall] [/log <file>]
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

enum {
    TRAILER = 32,
    MAX_PACKAGES = 64,
    FLAG_VITAL = 1,
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
    package_t   p[MAX_PACKAGES];
} chain_t;

static bool quiet, passive;

static void say(const wchar_t *title, const wchar_t *text, bool error) {
    if (quiet || (passive && !error)) return;
    MessageBoxW(NULL, text, title, MB_OK | (error ? MB_ICONERROR : MB_ICONINFORMATION));
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
    return true;
#undef STR
#undef NEED
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

static bool installed(const package_t *p) {
    wchar_t *pc = wide(p->product_code, p->product_code_len);
    bool yes = pc && MsiQueryProductStateW(pc) == INSTALLSTATE_DEFAULT;
    LocalFree(pc);
    return yes;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmdline, int show) {
    (void)inst;
    (void)prev;
    (void)show;
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool uninstall = false;
    const wchar_t *log = NULL;
    for (int i = 1; argv && i < argc; ++i) {
        if (lstrcmpiW(argv[i], L"/quiet") == 0 || lstrcmpiW(argv[i], L"/q") == 0) quiet = true;
        else if (lstrcmpiW(argv[i], L"/passive") == 0) passive = true;
        else if (lstrcmpiW(argv[i], L"/uninstall") == 0 || lstrcmpiW(argv[i], L"/x") == 0) uninstall = true;
        else if (lstrcmpiW(argv[i], L"/log") == 0 && i + 1 < argc) log = argv[++i];
        else {
            say(L"Setup", L"Usage: setup.exe [/quiet | /passive] [/uninstall] [/log <file>]", true);
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
        say(L"Setup", L"This setup program is damaged: download it again.", true);
        return EXIT_DAMAGED;
    }
    wchar_t *title = wide(c.name, c.name_len);
    if (title == NULL) return EXIT_DAMAGED;

    if ((c.flags & CHAIN_ELEVATE) && !elevated()) {
        const wchar_t *args = cmdline;
        CloseHandle(f);
        return relaunch(self, args);
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
            UINT r = pc ? MsiConfigureProductW(pc, INSTALLLEVEL_DEFAULT, INSTALLSTATE_ABSENT) : ERROR_OUTOFMEMORY;
            LocalFree(pc);
            if (r == ERROR_SUCCESS_REBOOT_REQUIRED) result = (int)r;
            else if (r != ERROR_SUCCESS && result == 0) result = (int)r;
        }
        if (result == 0 || result == ERROR_SUCCESS_REBOOT_REQUIRED) say(title, L"The programs were removed.", false);
        else say(title, L"A program could not be removed.", true);
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
        say(title, L"Setup could not create a temporary folder.", true);
        CloseHandle(f);
        return ERROR_CANNOT_MAKE;
    }
    for (uint32_t k = 0; k < c.count && result != EXIT_DAMAGED; ++k) {
        const package_t *p = &c.p[k];
        wchar_t *id = wide(p->id, p->id_len);
        wsprintfW(path, L"%s\\%s.msi", dir, id ? id : L"package");
        if (id == NULL || !extract(f, start + p->offset, p->size, p->sha256, path)) {
            say(title, L"This setup program is damaged: download it again.", true);
            result = EXIT_DAMAGED;
        }
        LocalFree(id);
    }
    CloseHandle(f);
    for (uint32_t k = 0; k < c.count && (result == 0 || result == ERROR_SUCCESS_REBOOT_REQUIRED); ++k) {
        const package_t *p = &c.p[k];
        if (installed(p)) continue;
        wchar_t *id = wide(p->id, p->id_len), *props = wide(p->properties, p->properties_len);
        wsprintfW(path, L"%s\\%s.msi", dir, id ? id : L"package");
        UINT r = id && props ? MsiInstallProductW(path, props) : ERROR_OUTOFMEMORY;
        if (r == ERROR_SUCCESS_REBOOT_REQUIRED) {
            result = (int)r;
        } else if (r != ERROR_SUCCESS && (p->flags & FLAG_VITAL)) {
            wsprintfW(msg, L"%s could not be installed (error %u). Nothing after it was installed.", id ? id : L"A package", r);
            say(title, msg, true);
            result = (int)r;
        }
        LocalFree(id);
        LocalFree(props);
    }
    for (uint32_t k = 0; k < c.count; ++k) {
        wchar_t *id = wide(c.p[k].id, c.p[k].id_len);
        wsprintfW(path, L"%s\\%s.msi", dir, id ? id : L"package");
        DeleteFileW(path);
        LocalFree(id);
    }
    RemoveDirectoryW(dir);
    if (result == 0 || result == ERROR_SUCCESS_REBOOT_REQUIRED) say(title, L"Setup finished.", false);
    LocalFree(title);
    LocalFree(manifest);
    return result;
}
