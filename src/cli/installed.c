// src/cli/installed.c - `rubrapack installed <{upgrade code}|family name>` (RFC-0031 B32): asks this
// Windows whether a product of that upgrade code (MsiEnumRelatedProducts: per machine, and per user
// for the user who runs it) or an app package of that family (GetPackagesByPackageFamily, for the
// user who runs it) is installed - the two looks the helper's [other-copy] preflight makes. One line
// for each: "msi <product code> <version> <name>" or "msix <package full name>", separated by tabs.
// Exit 0 when something is installed, 1 when nothing is. Both functions are found at run time
// (msi.dll from System32, kernel32), so rubrapack.exe still starts where one is missing.

#include "rubrapack/cli.h"
#include "rubrapack/buf.h"
#include "rubrapack/diag.h"
#include "rubrapack/inspect.h"
#include "rubrapack/mem.h"
#include "rubrapack/pal.h"
#include "rubrapack/text.h"

#include <string.h>

#include "proven/heap.h"

enum { NOTHING_INSTALLED = 1 };


// "{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}", either case; written to `out` in upper case.
static bool upgrade_code(const char *s, char out[39]) {
    if (strlen(s) != 38 || s[0] != '{' || s[37] != '}') return false;
    for (size_t k = 0; k < 38; ++k) {
        char c = s[k];
        if (k == 0 || k == 37) {
            out[k] = c;
            continue;
        }
        if (k == 9 || k == 14 || k == 19 || k == 24) {
            if (c != '-') return false;
        } else if (c >= 'a' && c <= 'f') {
            c = (char)(c - 32);
        } else if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) {
            return false;
        }
        out[k] = c;
    }
    out[38] = '\0';
    return true;
}

#if !defined(_WIN32)

int rp_cmd_installed(int argc, char **argv) {
    char code[39];
    if (argc != 3 || !(upgrade_code(argv[2], code) || rp_family_name_ok(argv[2]))) {
        (void)rp_cli_usage_error("installed");
        return RP_EXIT_USAGE;
    }
    rp_diag_error(RP_DIAG_NOT_IMPLEMENTED, "installed asks Windows what is installed; it runs on Windows only");
    return RP_EXIT_USAGE;
}

#else

#include <windows.h>

typedef UINT(WINAPI *enum_related_fn)(LPCWSTR, DWORD, DWORD, LPWSTR);
typedef UINT(WINAPI *product_info_fn)(LPCWSTR, LPCWSTR, LPWSTR, LPDWORD);
typedef LONG(WINAPI *by_family_fn)(PCWSTR, UINT32 *, PWSTR *, UINT32 *, WCHAR *);

// A UTF-16 string as UTF-8.
static void put_wide(rp_buf_t *o, const wchar_t *w) {
    size_t n = wcslen(w);
    rp_text_result_t need = rp_utf16_to_utf8((const proven_u16 *)w, n, NULL, 0);
    if (need.err != PROVEN_OK) return;
    proven_allocator_t heap = proven_heap_allocator();
    uint8_t *b = rp_mem_alloc(heap, need.units + 1, 1);
    if (b && rp_utf16_to_utf8((const proven_u16 *)w, n, b, need.units).err == PROVEN_OK) rp_buf_put(o, b, need.units);
    rp_mem_free(heap, b);
}

static void product_info(rp_buf_t *o, product_info_fn info, const wchar_t *product, const wchar_t *what) {
    wchar_t buf[1024];
    DWORD len = sizeof buf / sizeof buf[0];
    if (info(product, what, buf, &len) == ERROR_SUCCESS) put_wide(o, buf);
    else rp_buf_puts(o, "-");
}

static int msi_products(rp_buf_t *o, const char *code) {
    HMODULE msi = LoadLibraryExW(L"msi.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    enum_related_fn related = msi ? (enum_related_fn)(void (*)(void))GetProcAddress(msi, "MsiEnumRelatedProductsW") : NULL;
    product_info_fn info = msi ? (product_info_fn)(void (*)(void))GetProcAddress(msi, "MsiGetProductInfoW") : NULL;
    if (related == NULL || info == NULL) {
        rp_diag_error(RP_DIAG_INPUT, "Windows did not say what is installed for '%s'", code);
        if (msi) FreeLibrary(msi);
        return RP_EXIT_IO;
    }
    wchar_t wcode[39], product[39];
    for (int i = 0; i < 39; ++i) wcode[i] = (wchar_t)(unsigned char)code[i];
    int found = 0;
    for (DWORD i = 0; related(wcode, 0, i, product) == ERROR_SUCCESS; ++i) {
        rp_buf_puts(o, "msi\t");
        put_wide(o, product);
        rp_buf_puts(o, "\t");
        product_info(o, info, product, L"VersionString");
        rp_buf_puts(o, "\t");
        product_info(o, info, product, L"ProductName");
        rp_buf_puts(o, "\n");
        ++found;
    }
    FreeLibrary(msi);
    return found ? RP_EXIT_OK : NOTHING_INSTALLED;
}

static int msix_packages(rp_buf_t *o, const char *family) {
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    by_family_fn by_family = k32 ? (by_family_fn)(void (*)(void))GetProcAddress(k32, "GetPackagesByPackageFamily") : NULL;
    if (by_family == NULL) {
        rp_diag_error(RP_DIAG_INPUT, "Windows did not say what is installed for '%s'", family);
        return RP_EXIT_IO;
    }
    wchar_t wfamily[256];
    size_t n = strlen(family);
    if (n >= sizeof wfamily / sizeof wfamily[0]) return NOTHING_INSTALLED;
    for (size_t i = 0; i <= n; ++i) wfamily[i] = (wchar_t)(unsigned char)family[i];
    UINT32 count = 0, chars = 0;
    LONG rc = by_family(wfamily, &count, NULL, &chars, NULL);
    if (rc == ERROR_SUCCESS && count == 0) return NOTHING_INSTALLED;
    if (rc != ERROR_INSUFFICIENT_BUFFER) {
        rp_diag_error(RP_DIAG_INPUT, "Windows did not say what is installed for '%s'", family);
        return RP_EXIT_IO;
    }
    proven_allocator_t heap = proven_heap_allocator();
    PWSTR *names = rp_mem_alloc(heap, (size_t)count + 1, sizeof *names);
    WCHAR *buf = rp_mem_alloc(heap, (size_t)chars + 1, sizeof *buf);
    int out = RP_EXIT_IO;
    if (names && buf && by_family(wfamily, &count, names, &chars, buf) == ERROR_SUCCESS) {
        for (UINT32 i = 0; i < count; ++i) {
            rp_buf_puts(o, "msix\t");
            put_wide(o, names[i]);
            rp_buf_puts(o, "\n");
        }
        out = count ? RP_EXIT_OK : NOTHING_INSTALLED;
    } else {
        rp_diag_error(RP_DIAG_INPUT, "Windows did not say what is installed for '%s'", family);
    }
    rp_mem_free(heap, names);
    rp_mem_free(heap, buf);
    return out;
}

int rp_cmd_installed(int argc, char **argv) {
    char code[39];
    bool msi = argc == 3 && upgrade_code(argv[2], code);
    if (!msi && !(argc == 3 && rp_family_name_ok(argv[2]))) {
        (void)rp_cli_usage_error("installed");
        return RP_EXIT_USAGE;
    }
    rp_buf_t o = rp_buf_new(proven_heap_allocator(), 1u << 20);
    int rc = msi ? msi_products(&o, code) : msix_packages(&o, argv[2]);
    if (o.err != PROVEN_OK || (o.len && rp_pal_write(RP_OUT_STDOUT, o.data, o.len) != PROVEN_OK)) rc = RP_EXIT_IO;
    rp_buf_free(&o);
    return rc;
}

#endif
