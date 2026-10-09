// src/ca/rp_appx.h - registering and removing the identity package of [menu.*] items (plan
// 2026-10-09) through Windows' PackageManager, from C. Included by the helper DLL.
//
// Windows 11 shows in its context menu only what a package with identity declares. An MSI gets one
// by registering a small package (a manifest and logos) with the program's folder as its external
// location:
//   per machine  StagePackageByUriAsync(file, { ExternalLocationUri, AllowUnsigned }) and
//                ProvisionPackageForAllUsersAsync(family): every user has it (others at their next
//                sign-in); removal: RemovePackageAsync(full name, RemoveForAllUsers) and
//                DeprovisionPackageForAllUsersAsync(family)
//   per user     AddPackageByUriAsync(file, { ExternalLocationUri, AllowUnsigned });
//                removal: RemovePackageAsync(full name)
// The interfaces are not in the toolchain's headers; their IDs and method positions are from
// Windows.Management.winmd (Windows 11 26200): a position counts from QueryInterface = 0, so 6 is
// the first method after IInspectable's. Everything is called through those positions.
//
// A failure is never an installation failure: the classic menu works without the package. On a
// Windows without these interfaces (before Windows 10 2004) nothing is registered.

#ifndef RUBRAPACK_APPX_H
#define RUBRAPACK_APPX_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>
#include <roapi.h>
#include <winstring.h>

static const GUID APPX_IID_ASYNC_INFO = { 0x00000036, 0x0000, 0x0000, { 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
static const GUID APPX_IID_URI_FACTORY = { 0x44A9796F, 0x723E, 0x4FDF, { 0xA2, 0x18, 0x03, 0x3E, 0x75, 0xB0, 0xC0, 0x84 } };
static const GUID APPX_IID_PM = { 0x9A7D4B65, 0x5E8F, 0x4FC7, { 0xA2, 0xE5, 0x7F, 0x69, 0x25, 0xCB, 0x8B, 0x53 } };
static const GUID APPX_IID_PM2 = { 0xF7AAD08D, 0x0840, 0x46F2, { 0xB5, 0xD8, 0xCA, 0xD4, 0x76, 0x93, 0xA0, 0x95 } };
static const GUID APPX_IID_PM6 = { 0x0847E909, 0x53CD, 0x4E4F, { 0x83, 0x2E, 0x57, 0xD1, 0x80, 0xF6, 0xE4, 0x47 } };
static const GUID APPX_IID_PM8 = { 0xB8575330, 0x1298, 0x4EE2, { 0x80, 0xEE, 0x7F, 0x65, 0x9C, 0x5D, 0x27, 0x82 } };
static const GUID APPX_IID_PM9 = { 0x1AA79035, 0xCC71, 0x4B2E, { 0x80, 0xA6, 0xC7, 0x04, 0x1D, 0x85, 0x79, 0xA7 } };
static const GUID APPX_IID_STAGE_OPTIONS = { 0x0B110C9C, 0xB95D, 0x4C56, { 0xBD, 0x36, 0x6D, 0x65, 0x68, 0x00, 0xD0, 0x6B } };
static const GUID APPX_IID_ADD_OPTIONS = { 0x05CEE018, 0xF68F, 0x422B, { 0x95, 0xA4, 0x66, 0x67, 0x9E, 0xC7, 0x7F, 0xC0 } };

enum {
    APPX_PM_REMOVE = 8,             // IPackageManager::RemovePackageAsync(full name)
    APPX_PM2_REMOVE = 6,            // IPackageManager2::RemovePackageAsync(full name, options)
    APPX_PM6_PROVISION = 6,         // IPackageManager6::ProvisionPackageForAllUsersAsync(family)
    APPX_PM8_DEPROVISION = 6,       // IPackageManager8::DeprovisionPackageForAllUsersAsync(family)
    APPX_PM9_ADD = 7,               // IPackageManager9::AddPackageByUriAsync(uri, options)
    APPX_PM9_STAGE = 8,             // IPackageManager9::StagePackageByUriAsync(uri, options)
    APPX_STAGE_PUT_EXTERNAL = 13,   // IStagePackageOptions::put_ExternalLocationUri
    APPX_STAGE_PUT_UNSIGNED = 27,   // IStagePackageOptions::put_AllowUnsigned
    APPX_ADD_PUT_EXTERNAL = 13,     // IAddPackageOptions::put_ExternalLocationUri
    APPX_ADD_PUT_UNSIGNED = 33,     // IAddPackageOptions::put_AllowUnsigned
    APPX_URI_CREATE = 6,            // IUriRuntimeClassFactory::CreateUri
    APPX_INFO_STATUS = 7,           // IAsyncInfo::get_Status (0 started, 1 completed, 2 canceled, 3 error)
    APPX_INFO_ERROR = 8,            // IAsyncInfo::get_ErrorCode
    APPX_REMOVE_FOR_ALL_USERS = 0x80000,
    APPX_WAIT_MS = 180000,
};

// The method at `slot` of a COM object.
#define APPX_SLOT(obj, slot) ((*(void ***)(obj))[slot])

static void appx_release(void *obj) {
    if (obj) ((ULONG(STDMETHODCALLTYPE *)(void *))APPX_SLOT(obj, 2))(obj);
}

static HRESULT appx_query(void *obj, const GUID *iid, void **out) {
    *out = NULL;
    return ((HRESULT(STDMETHODCALLTYPE *)(void *, const GUID *, void **))APPX_SLOT(obj, 0))(obj, iid, out);
}

// An object of a WinRT class, as the interface `iid`.
static HRESULT appx_new(const wchar_t *cls, const GUID *iid, void **out) {
    *out = NULL;
    HSTRING name = NULL;
    HRESULT hr = WindowsCreateString(cls, (UINT32)wcslen(cls), &name);
    if (FAILED(hr)) return hr;
    IInspectable *obj = NULL;
    hr = RoActivateInstance(name, &obj);
    WindowsDeleteString(name);
    if (FAILED(hr)) return hr;
    hr = appx_query(obj, iid, out);
    appx_release(obj);
    return hr;
}

// A Windows.Foundation.Uri for a file or folder path: "file:///C:/a%20b/c".
static HRESULT appx_uri(const wchar_t *path, void **out) {
    *out = NULL;
    static const wchar_t hex[] = L"0123456789ABCDEF";
    size_t n = wcslen(path);
    wchar_t *text = HeapAlloc(GetProcessHeap(), 0, (n * 9 + 16) * sizeof *text);
    if (text == NULL) return E_OUTOFMEMORY;
    size_t w = 0;
    memcpy(text, L"file:///", 8 * sizeof *text);
    w = 8;
    for (size_t i = 0; i < n; ++i) {
        // One code point as UTF-8, each byte kept or written as %XX.
        uint32_t cp = path[i];
        if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < n && path[i + 1] >= 0xDC00 && path[i + 1] < 0xE000) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (path[i + 1] - 0xDC00);
            ++i;
        }
        uint8_t b[4];
        int nb = 0;
        if (cp < 0x80) b[nb++] = (uint8_t)cp;
        else if (cp < 0x800) b[nb++] = (uint8_t)(0xC0 | cp >> 6), b[nb++] = (uint8_t)(0x80 | (cp & 63));
        else if (cp < 0x10000) b[nb++] = (uint8_t)(0xE0 | cp >> 12), b[nb++] = (uint8_t)(0x80 | (cp >> 6 & 63)), b[nb++] = (uint8_t)(0x80 | (cp & 63));
        else b[nb++] = (uint8_t)(0xF0 | cp >> 18), b[nb++] = (uint8_t)(0x80 | (cp >> 12 & 63)), b[nb++] = (uint8_t)(0x80 | (cp >> 6 & 63)), b[nb++] = (uint8_t)(0x80 | (cp & 63));
        for (int k = 0; k < nb; ++k) {
            uint8_t c = b[k];
            bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~' ||
                         c == ':' || c == '/';
            if (c == '\\') text[w++] = L'/';
            else if (plain) text[w++] = c;
            else text[w++] = L'%', text[w++] = hex[c >> 4], text[w++] = hex[c & 15];
        }
    }
    text[w] = 0;
    HSTRING cls = NULL, s = NULL;
    void *factory = NULL;
    static const wchar_t uri_class[] = L"Windows.Foundation.Uri";
    HRESULT hr = WindowsCreateString(uri_class, (UINT32)wcslen(uri_class), &cls);
    if (SUCCEEDED(hr)) hr = RoGetActivationFactory(cls, &APPX_IID_URI_FACTORY, &factory);
    if (SUCCEEDED(hr)) hr = WindowsCreateString(text, (UINT32)w, &s);
    if (SUCCEEDED(hr)) hr = ((HRESULT(STDMETHODCALLTYPE *)(void *, HSTRING, void **))APPX_SLOT(factory, APPX_URI_CREATE))(factory, s, out);
    if (s) WindowsDeleteString(s);
    if (cls) WindowsDeleteString(cls);
    appx_release(factory);
    HeapFree(GetProcessHeap(), 0, text);
    return hr;
}

// Waits for an asynchronous operation and releases it; its result as an HRESULT.
static HRESULT appx_wait(void *op) {
    if (op == NULL) return E_FAIL;
    void *info = NULL;
    HRESULT hr = appx_query(op, &APPX_IID_ASYNC_INFO, &info);
    if (SUCCEEDED(hr)) {
        int status = 0;
        for (DWORD waited = 0; waited < APPX_WAIT_MS; waited += 100) {
            hr = ((HRESULT(STDMETHODCALLTYPE *)(void *, int *))APPX_SLOT(info, APPX_INFO_STATUS))(info, &status);
            if (FAILED(hr) || status != 0) break;
            Sleep(100);
        }
        if (SUCCEEDED(hr) && status == 0) hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        else if (SUCCEEDED(hr) && status != 1) {
            HRESULT code = E_FAIL;
            ((HRESULT(STDMETHODCALLTYPE *)(void *, HRESULT *))APPX_SLOT(info, APPX_INFO_ERROR))(info, &code);
            hr = FAILED(code) ? code : E_FAIL;
        }
        appx_release(info);
    }
    appx_release(op);
    return hr;
}

static HRESULT appx_hstring_call(void *obj, int slot, const wchar_t *text) {
    HSTRING s = NULL;
    void *op = NULL;
    HRESULT hr = WindowsCreateString(text, (UINT32)wcslen(text), &s);
    if (SUCCEEDED(hr)) hr = ((HRESULT(STDMETHODCALLTYPE *)(void *, HSTRING, void **))APPX_SLOT(obj, slot))(obj, s, &op);
    if (s) WindowsDeleteString(s);
    return SUCCEEDED(hr) ? appx_wait(op) : hr;
}

typedef struct {
    const wchar_t *package;         // the .msix file (add)
    const wchar_t *external;        // the program's folder (add)
    const wchar_t *full_name;       // remove
    const wchar_t *family;          // provision, deprovision
    bool           machine, allow_unsigned, remove;
    HRESULT        result;
    const wchar_t *step;            // what failed
} appx_job_t;

// The work, on a thread of its own in the multithreaded apartment (the caller's thread may be in
// another one, and the operations are waited for by polling).
static DWORD WINAPI appx_thread(void *arg) {
    appx_job_t *j = arg;
    HRESULT init = RoInitialize(RO_INIT_MULTITHREADED);
    void *pm = NULL, *pm2 = NULL, *pm6 = NULL, *pm8 = NULL, *pm9 = NULL, *uri = NULL, *ext = NULL, *opt = NULL, *op = NULL;
    j->step = L"PackageManager";
    HRESULT hr = appx_new(L"Windows.Management.Deployment.PackageManager", &APPX_IID_PM, &pm);
    if (SUCCEEDED(hr) && j->remove) {
        j->step = L"RemovePackageAsync";
        if (j->machine) {
            hr = appx_query(pm, &APPX_IID_PM2, &pm2);
            HSTRING s = NULL;
            if (SUCCEEDED(hr)) hr = WindowsCreateString(j->full_name, (UINT32)wcslen(j->full_name), &s);
            if (SUCCEEDED(hr)) {
                hr = ((HRESULT(STDMETHODCALLTYPE *)(void *, HSTRING, UINT32, void **))APPX_SLOT(pm2, APPX_PM2_REMOVE))(pm2, s, APPX_REMOVE_FOR_ALL_USERS, &op);
            }
            if (s) WindowsDeleteString(s);
            if (SUCCEEDED(hr)) hr = appx_wait(op);
            // Not provisioned any more either (a failure here leaves only a record: the package is gone).
            if (SUCCEEDED(appx_query(pm, &APPX_IID_PM8, &pm8))) (void)appx_hstring_call(pm8, APPX_PM8_DEPROVISION, j->family);
        } else {
            hr = appx_hstring_call(pm, APPX_PM_REMOVE, j->full_name);
        }
    } else if (SUCCEEDED(hr)) {
        j->step = L"IPackageManager9";
        hr = appx_query(pm, &APPX_IID_PM9, &pm9);
        j->step = L"Uri";
        if (SUCCEEDED(hr)) hr = appx_uri(j->package, &uri);
        if (SUCCEEDED(hr)) hr = appx_uri(j->external, &ext);
        if (SUCCEEDED(hr) && j->machine) {
            j->step = L"StagePackageOptions";
            hr = appx_new(L"Windows.Management.Deployment.StagePackageOptions", &APPX_IID_STAGE_OPTIONS, &opt);
            if (SUCCEEDED(hr)) hr = ((HRESULT(STDMETHODCALLTYPE *)(void *, void *))APPX_SLOT(opt, APPX_STAGE_PUT_EXTERNAL))(opt, ext);
            if (SUCCEEDED(hr) && j->allow_unsigned) hr = ((HRESULT(STDMETHODCALLTYPE *)(void *, boolean))APPX_SLOT(opt, APPX_STAGE_PUT_UNSIGNED))(opt, 1);
            j->step = L"StagePackageByUriAsync";
            if (SUCCEEDED(hr)) hr = ((HRESULT(STDMETHODCALLTYPE *)(void *, void *, void *, void **))APPX_SLOT(pm9, APPX_PM9_STAGE))(pm9, uri, opt, &op);
            if (SUCCEEDED(hr)) hr = appx_wait(op);
            j->step = L"ProvisionPackageForAllUsersAsync";
            if (SUCCEEDED(hr)) hr = appx_query(pm, &APPX_IID_PM6, &pm6);
            if (SUCCEEDED(hr)) hr = appx_hstring_call(pm6, APPX_PM6_PROVISION, j->family);
        } else if (SUCCEEDED(hr)) {
            j->step = L"AddPackageOptions";
            hr = appx_new(L"Windows.Management.Deployment.AddPackageOptions", &APPX_IID_ADD_OPTIONS, &opt);
            if (SUCCEEDED(hr)) hr = ((HRESULT(STDMETHODCALLTYPE *)(void *, void *))APPX_SLOT(opt, APPX_ADD_PUT_EXTERNAL))(opt, ext);
            if (SUCCEEDED(hr) && j->allow_unsigned) hr = ((HRESULT(STDMETHODCALLTYPE *)(void *, boolean))APPX_SLOT(opt, APPX_ADD_PUT_UNSIGNED))(opt, 1);
            j->step = L"AddPackageByUriAsync";
            if (SUCCEEDED(hr)) hr = ((HRESULT(STDMETHODCALLTYPE *)(void *, void *, void *, void **))APPX_SLOT(pm9, APPX_PM9_ADD))(pm9, uri, opt, &op);
            if (SUCCEEDED(hr)) hr = appx_wait(op);
        }
    }
    void *all[] = { opt, ext, uri, pm9, pm8, pm6, pm2, pm };
    for (size_t i = 0; i < sizeof all / sizeof all[0]; ++i) appx_release(all[i]);
    if (SUCCEEDED(init)) RoUninitialize();
    j->result = hr;
    return 0;
}

// Runs the job; its result (and j->step, the step that failed).
static HRESULT appx_run(appx_job_t *j) {
    j->result = E_FAIL;
    j->step = L"thread";
    HANDLE t = CreateThread(NULL, 0, appx_thread, j, 0, NULL);
    if (t == NULL) return HRESULT_FROM_WIN32(GetLastError());
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    return j->result;
}

#endif
