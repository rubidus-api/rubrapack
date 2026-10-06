// src/launch/rubrapack_launch.c - the launcher of an MSIX application whose source sets environment
// variables (RFC-0019). An MSIX cannot change the machine's environment, so the variables are given
// to the application's own processes: the manifest names this program as the application's
// executable (rubrapack\<AppId>.exe), and it
//   reads rubrapack\launch.txt beside it, the block of its own name:
//     app <AppId>
//     target <package path of the program>
//     set NAME=value | append NAME=value | prepend NAME=value
//   where %PKG% in a value is the package's folder and %NAME% the variable's value at that moment;
//   sets them (append and prepend join with ';' to what is there);
//   starts the program with the command line it was given, in the program's folder, and returns its
//   exit code.
// Built twice: a console program for console targets (the console is shared) and a windows one.
// It needs kernel32 (and user32 for the message box of the windows one).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdint.h>
#include <string.h>

enum { MAX_TEXT = 1 << 20, MAX_VALUE = 32767 };

static void fail(const wchar_t *what) {
#if defined(RP_LAUNCH_CONSOLE)
    HANDLE e = GetStdHandle(STD_ERROR_HANDLE);
    DWORD n;
    WriteConsoleW(e, L"rubrapack launcher: ", 20, &n, NULL);
    WriteConsoleW(e, what, (DWORD)lstrlenW(what), &n, NULL);
    WriteConsoleW(e, L"\r\n", 2, &n, NULL);
#else
    MessageBoxW(NULL, what, L"rubrapack launcher", MB_OK | MB_ICONERROR);
#endif
}

// One line of the text (UTF-8, LF or CRLF) as UTF-16, or 0 at the end.
static size_t next_line(const char *t, size_t len, size_t *at, wchar_t *out, size_t cap) {
    if (*at >= len) return 0;
    size_t s = *at, e = s;
    while (e < len && t[e] != '\n') ++e;
    *at = e + 1;
    if (e > s && t[e - 1] == '\r') --e;
    int n = MultiByteToWideChar(CP_UTF8, 0, t + s, (int)(e - s), out, (int)cap - 1);
    if (n < 0) n = 0;
    out[n] = 0;
    return (size_t)n + 1;
}

static bool starts(const wchar_t *s, const wchar_t *p) { return wcsncmp(s, p, wcslen(p)) == 0; }

// %PKG% becomes the package folder; the rest goes through ExpandEnvironmentStrings.
static bool expand(const wchar_t *v, const wchar_t *pkg, wchar_t *out, size_t cap) {
    static wchar_t tmp[MAX_VALUE + 1];
    size_t w = 0;
    for (const wchar_t *p = v; *p;) {
        if (starts(p, L"%PKG%")) {
            size_t n = wcslen(pkg);
            if (w + n >= MAX_VALUE) return false;
            memcpy(tmp + w, pkg, n * sizeof *tmp);
            w += n;
            p += 5;
        } else {
            if (w + 1 >= MAX_VALUE) return false;
            tmp[w++] = *p++;
        }
    }
    tmp[w] = 0;
    DWORD n = ExpandEnvironmentStringsW(tmp, out, (DWORD)cap);
    return n > 0 && n <= cap;
}

static int run(void) {
    static wchar_t self[MAX_PATH * 4], dir[MAX_PATH * 4], pkg[MAX_PATH * 4], name[256], path[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(NULL, self, (DWORD)(sizeof self / sizeof *self));
    if (n == 0 || n >= sizeof self / sizeof *self) return fail(L"cannot find itself"), 1;
    wcscpy(dir, self);
    wchar_t *slash = wcsrchr(dir, L'\\');
    if (slash == NULL) return fail(L"cannot find its folder"), 1;
    wcscpy(name, slash + 1);
    *slash = 0;                                     // dir: ...\rubrapack
    wcscpy(pkg, dir);
    slash = wcsrchr(pkg, L'\\');
    if (slash == NULL) return fail(L"cannot find the package folder"), 1;
    *slash = 0;                                     // pkg: the package's folder
    wchar_t *dot = wcsrchr(name, L'.');
    if (dot) *dot = 0;                              // name: the AppId

    // launch.txt
    wcscpy(path, dir);
    wcscat(path, L"\\launch.txt");
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return fail(L"cannot read rubrapack\\launch.txt"), 1;
    static char text[MAX_TEXT];
    DWORD got = 0;
    BOOL ok = ReadFile(f, text, MAX_TEXT, &got, NULL);
    CloseHandle(f);
    if (!ok) return fail(L"cannot read rubrapack\\launch.txt"), 1;

    static wchar_t line[MAX_VALUE + 64], value[MAX_VALUE + 1], old[MAX_VALUE + 1], joined[2 * MAX_VALUE + 2], target[MAX_PATH * 4];
    bool mine = false, have_target = false;
    size_t at = 0;
    while (next_line(text, got, &at, line, sizeof line / sizeof *line)) {
        if (starts(line, L"app ")) {
            mine = lstrcmpiW(line + 4, name) == 0;
            continue;
        }
        if (!mine) continue;
        if (starts(line, L"target ")) {
            if (wcslen(pkg) + wcslen(line + 7) + 2 > sizeof target / sizeof *target) return fail(L"the target's path is too long"), 1;
            wcscpy(target, pkg);
            wcscat(target, L"\\");
            wcscat(target, line + 7);
            have_target = true;
            continue;
        }
        int mode = starts(line, L"set ") ? 0 : starts(line, L"append ") ? 1 : starts(line, L"prepend ") ? 2 : -1;
        if (mode < 0) continue;
        wchar_t *var = line + (mode == 0 ? 4 : mode == 1 ? 7 : 8);
        wchar_t *eq = wcschr(var, L'=');
        if (eq == NULL) continue;
        *eq = 0;
        if (!expand(eq + 1, pkg, value, sizeof value / sizeof *value)) return fail(L"a value is too long"), 1;
        DWORD have = mode ? GetEnvironmentVariableW(var, old, (DWORD)(sizeof old / sizeof *old)) : 0;
        if (have >= sizeof old / sizeof *old) have = 0;
        if (mode == 0 || have == 0) {
            SetEnvironmentVariableW(var, value);
        } else {
            if (mode == 1) {
                wcscpy(joined, old);
                wcscat(joined, L";");
                wcscat(joined, value);
            } else {
                wcscpy(joined, value);
                wcscat(joined, L";");
                wcscat(joined, old);
            }
            SetEnvironmentVariableW(var, joined);
        }
    }
    if (!have_target) return fail(L"rubrapack\\launch.txt names no program for this application"), 1;

    // The command line: the target, then what follows our own program name.
    const wchar_t *cl = GetCommandLineW();
    if (*cl == L'"') {
        ++cl;
        while (*cl && *cl != L'"') ++cl;
        if (*cl) ++cl;
    } else {
        while (*cl && *cl != L' ' && *cl != L'\t') ++cl;
    }
    static wchar_t cmd[32768];
    size_t tl = wcslen(target), rl = wcslen(cl);
    if (tl + rl + 3 >= sizeof cmd / sizeof *cmd) return fail(L"the command line is too long"), 1;
    cmd[0] = L'"';
    wcscpy(cmd + 1, target);
    wcscat(cmd, L"\"");
    wcscat(cmd, cl);
    static wchar_t cwd[MAX_PATH * 4];
    wcscpy(cwd, target);
    slash = wcsrchr(cwd, L'\\');
    if (slash) *slash = 0;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    // A console target gets this program's standard handles, so a redirection or a pipe reaches it.
#if defined(RP_LAUNCH_CONSOLE)
    const BOOL inherit = TRUE;
#else
    const BOOL inherit = FALSE;
#endif
    if (!CreateProcessW(target, cmd, NULL, NULL, inherit, 0, NULL, cwd, &si, &pi)) return fail(L"cannot start the program"), 1;
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    return (int)code;
}

#if defined(RP_LAUNCH_CONSOLE)
int wmain(void) { return run(); }
#else
int WINAPI wWinMain(HINSTANCE h, HINSTANCE p, PWSTR c, int s) {
    (void)h;
    (void)p;
    (void)c;
    (void)s;
    return run();
}
#endif
