// nob.c - rubrapack build driver. Self-contained: the C standard library and mkdir, no
// third-party headers.
//
//   cc -std=c23 -o nob nob.c
//   ./nob                        build build/<target>/rubrapack for this host
//   ./nob --target=win64         cross-build build/win64/rubrapack.exe (MinGW-w64)
//   ./nob --sanitize             build with AddressSanitizer + UBSan into build/<target>-asan/
//   ./nob --debug                -O0 -g instead of -O2
//   ./nob clean                  remove build/
//
// Compilers: $CC (default cc; gcc on a Windows host) for the host, $RUBRAPACK_WIN64_CC
// (default x86_64-w64-mingw32-gcc) for --target=win64.
//
// Two flag sets. rubrapack's own sources: -std=c23 -Wall -Wextra -pedantic -Werror. The
// vendored proven_c_lib: proven's own set (no -pedantic, which rejects its __int128), see
// vendor/proven/VENDORED.md. Objects go to build/<target>/obj/; the process entry object's
// name starts with "entry_" so a test build can link every other object with its own main.

#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#define make_dir(p) _mkdir(p)
#else
#include <sys/stat.h>
#define make_dir(p) mkdir((p), 0777)
#endif

// rubrapack sources shared by every platform.
static const char *const core_sources[] = {
    "src/cli/cli.c",
    "src/core/diag.c",
    "src/core/limits.c",
    "src/text/utf.c",
};

static const char *const posix_sources[] = {
    "src/pal/posix/pal_posix.c",
    "src/pal/posix/entry_posix.c",
};

static const char *const win32_sources[] = {
    "src/pal/win32/pal_win32.c",
    "src/pal/win32/entry_win32.c",
};

// The parts of proven_c_lib rubrapack links, relative to vendor/proven/.
static const char *const proven_sources[] = {
    "src/proven/buffer.c",
    "src/proven/heap.c",
    "src/proven/memory.c",
    "src/proven/u16str.c",
    "src/proven/u8str.c",
    "platform/proven_sys_mem.c",
};

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

typedef enum { KIND_POSIX, KIND_WIN32 } kind_t;

typedef struct {
    const char *name;       // build/<name>/
    const char *cc;
    kind_t      kind;
    bool        sanitize;
    bool        debug;
} target_t;

static char cmd[16384];

static int run(const char *command) {
    printf("%s\n", command);
    fflush(stdout);
    int rc = system(command);
    return rc == 0 ? 0 : 1;
}

static int mkdirs(const char *path) {
    char buf[512];
    size_t n = strlen(path);
    if (n + 1 > sizeof buf) return 1;
    memcpy(buf, path, n + 1);
    for (size_t i = 1; i <= n; ++i) {
        if (buf[i] == '/' || buf[i] == '\0') {
            char saved = buf[i];
            buf[i] = '\0';
            if (make_dir(buf) != 0 && errno != EEXIST) {
                fprintf(stderr, "nob: cannot create %s\n", buf);
                return 1;
            }
            buf[i] = saved;
        }
    }
    return 0;
}

// <dir>/<source path with '/' as '_'>.o, except the process entry, which is <dir>/entry_*.o.
static void object_path(char *out, size_t cap, const char *dir, const char *source) {
    const char *base = strrchr(source, '/');
    base = base ? base + 1 : source;
    char flat[256];
    size_t o = 0;
    if (strncmp(base, "entry_", 6) == 0) {
        snprintf(flat, sizeof flat, "%s", base);
        o = strlen(flat);
    } else {
        for (const char *p = source; *p && o + 1 < sizeof flat; ++p) flat[o++] = (*p == '/') ? '_' : *p;
        flat[o] = '\0';
    }
    if (o > 2 && flat[o - 2] == '.' && flat[o - 1] == 'c') flat[o - 2] = '\0';
    snprintf(out, cap, "%s/%s.o", dir, flat);
}

static const char *opt_flags(const target_t *t) {
    if (t->sanitize) return "-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all";
    if (t->debug) return "-O0 -g";
    return "-O2";
}

static int compile(const target_t *t, const char *flags, const char *source, const char *object) {
    int n = snprintf(cmd, sizeof cmd, "%s %s %s -c %s -o %s", t->cc, flags, opt_flags(t), source, object);
    if (n < 0 || (size_t)n >= sizeof cmd) return 1;
    return run(cmd);
}

static int append(size_t *len, const char *text) {
    size_t n = strlen(text);
    if (*len + n + 2 > sizeof cmd) return 1;
    cmd[(*len)++] = ' ';
    memcpy(cmd + *len, text, n + 1);
    *len += n;
    return 0;
}

static int build(const target_t *t) {
    char dir[128], obj_dir[160], exe[160], object[512], source[256];
    snprintf(dir, sizeof dir, "build/%s%s", t->name, t->sanitize ? "-asan" : "");
    snprintf(obj_dir, sizeof obj_dir, "%s/obj", dir);
    snprintf(exe, sizeof exe, "%s/rubrapack%s", dir, t->kind == KIND_WIN32 ? ".exe" : "");
    if (mkdirs(obj_dir) != 0) return 1;

    const char *own =
        "-std=c23 -Wall -Wextra -pedantic -Werror -Iinclude -Ivendor/proven/include";
    const char *vendor =
        "-std=c23 -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror "
        "-Ivendor/proven/include -Ivendor/proven/platform";

    const char *const *pal = t->kind == KIND_WIN32 ? win32_sources : posix_sources;
    size_t pal_count = t->kind == KIND_WIN32 ? COUNT(win32_sources) : COUNT(posix_sources);

    char link[16384];
    size_t link_len = 0;
    link[0] = '\0';

    for (size_t i = 0; i < COUNT(core_sources) + pal_count + COUNT(proven_sources); ++i) {
        bool is_vendor = i >= COUNT(core_sources) + pal_count;
        if (i < COUNT(core_sources)) {
            snprintf(source, sizeof source, "%s", core_sources[i]);
        } else if (!is_vendor) {
            snprintf(source, sizeof source, "%s", pal[i - COUNT(core_sources)]);
        } else {
            snprintf(source, sizeof source, "vendor/proven/%s",
                     proven_sources[i - COUNT(core_sources) - pal_count]);
        }
        object_path(object, sizeof object, obj_dir, source);
        if (compile(t, is_vendor ? vendor : own, source, object) != 0) return 1;
        int n = snprintf(link + link_len, sizeof link - link_len, " %s", object);
        if (n < 0 || (size_t)n >= sizeof link - link_len) return 1;
        link_len += (size_t)n;
    }

    int n = snprintf(cmd, sizeof cmd, "%s %s%s -o %s", t->cc, opt_flags(t), link, exe);
    if (n < 0 || (size_t)n >= sizeof cmd) return 1;
    size_t len = (size_t)n;
    if (t->kind == KIND_WIN32 && append(&len, "-static -municode -lbcrypt") != 0) return 1;
    if (run(cmd) != 0) return 1;
    printf("nob: built %s\n", exe);
    return 0;
}

int main(int argc, char **argv) {
#if defined(_WIN32)
    target_t t = { .name = "native", .cc = "gcc", .kind = KIND_WIN32 };
#else
    target_t t = { .name = "native", .cc = "cc", .kind = KIND_POSIX };
#endif
    const char *env_cc = getenv("CC");
    if (env_cc != NULL && env_cc[0] != '\0') t.cc = env_cc;

    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        if (strcmp(a, "clean") == 0) {
#if defined(_WIN32)
            return run("if exist build rmdir /s /q build");
#else
            return run("rm -rf build");
#endif
        } else if (strcmp(a, "--target=native") == 0) {
            // default
        } else if (strcmp(a, "--target=win64") == 0) {
            const char *wcc = getenv("RUBRAPACK_WIN64_CC");
            t.name = "win64";
            t.kind = KIND_WIN32;
            t.cc = (wcc != NULL && wcc[0] != '\0') ? wcc : "x86_64-w64-mingw32-gcc";
        } else if (strcmp(a, "--sanitize") == 0) {
            t.sanitize = true;
        } else if (strcmp(a, "--debug") == 0) {
            t.debug = true;
        } else {
            fprintf(stderr, "nob: unknown argument '%s'\n"
                            "usage: ./nob [--target=native|win64] [--sanitize] [--debug] | clean\n", a);
            return 2;
        }
    }
    return build(&t);
}
