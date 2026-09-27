// nob.c - rubrapack build driver. Self-contained: the C standard library and mkdir, no
// third-party headers.
//
//   cc -std=c23 -o nob nob.c
//   ./nob                        build build/<target>/rubrapack for this host
//   ./nob --target=win64         cross-build build/win64/rubrapack.exe (MinGW-w64)
//   ./nob --sanitize             build with AddressSanitizer + UBSan into build/<target>-asan/
//   ./nob --debug                -O0 -g instead of -O2
//   ./nob clean                  remove build/
//   ./nob parts                  rebuild resources/bin/rubrapack_ca-{x64,x86,arm64}.dll and SHA256SUMS
//                                (needs llvm-mingw: $RUBRAPACK_LLVM_MINGW/bin, RFC-0001 16 Q5)
//
// Every build embeds resources/bin/rubrapack_ca-*.dll as C arrays (build/<target>/gen/ca_parts.c):
// the helper custom-action DLL goes into packages that need it. A missing part is an empty array.
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
    "src/cli/build.c",
    "src/cli/cli.c",
    "src/cli/inspect.c",
    "src/cli/lint.c",
    "src/cli/extract.c",
    "src/cli/new.c",
    "src/codec/deflate.c",
    "src/codec/md5.c",
    "src/container/cab.c",
    "src/container/cfb.c",
    "src/container/cfb_write.c",
    "src/core/buf.c",
    "src/core/diag.c",
    "src/core/limits.c",
    "src/core/srcdiag.c",
    "src/lang/toml.c",
    "src/model/ident.c",
    "src/model/ir.c",
    "src/msi/db.c",
    "src/msi/db_write.c",
    "src/msi/lint.c",
    "src/msi/lower.c",
    "src/msi/suminfo.c",
    "src/msi/ui.c",
    "src/msi/view.c",
    "src/pe/pe.c",
    "src/text/utf.c",
    "src/text/nfc.c",
    "src/text/nfc_tables.c",
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
    "src/proven/hash.c",
    "src/proven/heap.c",
    "src/proven/memory.c",
    "src/proven/random.c",
    "src/proven/u16str.c",
    "src/proven/u8str.c",
    "platform/proven_sys_mem.c",
    "platform/proven_sys_random.c",
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

// ---- SHA-256 (FIPS 180-4), for resources/bin/SHA256SUMS -------------------------------------

static const unsigned int sha_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha_block(unsigned int h[8], const unsigned char *p) {
    unsigned int w[64];
    for (int i = 0; i < 16; ++i) w[i] = (unsigned int)p[4 * i] << 24 | (unsigned int)p[4 * i + 1] << 16 | (unsigned int)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; ++i) {
        unsigned int s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        unsigned int s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    unsigned int a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
    for (int i = 0; i < 64; ++i) {
        unsigned int t1 = k + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + sha_k[i] + w[i];
        unsigned int t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        k = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += k;
}

static void sha256_hex(const unsigned char *data, size_t len, char out[65]) {
    unsigned int h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    size_t i = 0;
    for (; i + 64 <= len; i += 64) sha_block(h, data + i);
    unsigned char tail[128] = { 0 };
    size_t rest = len - i, n = rest < 56 ? 64 : 128;
    memcpy(tail, data + i, rest);
    tail[rest] = 0x80;
    unsigned long long bits = (unsigned long long)len * 8;
    for (int b = 0; b < 8; ++b) tail[n - 1 - b] = (unsigned char)(bits >> (8 * b));
    sha_block(h, tail);
    if (n == 128) sha_block(h, tail + 64);
    for (int b = 0; b < 8; ++b) snprintf(out + 8 * b, 9, "%08x", h[b]);
}

// ---- helper DLL parts --------------------------------------------------------------------------

static const char *const part_archs[][2] = { { "x64", "x86_64" }, { "x86", "i686" }, { "arm64", "aarch64" } };

static unsigned char *read_all(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) return NULL;
    size_t cap = 1 << 16, n = 0;
    unsigned char *buf = malloc(cap);
    while (buf) {
        size_t got = fread(buf + n, 1, cap - n, f);
        n += got;
        if (n < cap) break;
        unsigned char *nb = realloc(buf, cap *= 2);
        if (nb == NULL) {
            free(buf);
            buf = NULL;
        }
        buf = nb;
    }
    fclose(f);
    *len = n;
    return buf;
}

// ./nob parts: the helper DLL for the three architectures, reproducibly (no timestamp, no paths,
// stripped), and their SHA-256 sums.
static int parts(void) {
    const char *root = getenv("RUBRAPACK_LLVM_MINGW");
    if (root == NULL || root[0] == '\0') {
        fprintf(stderr, "nob parts: set RUBRAPACK_LLVM_MINGW to an llvm-mingw toolchain folder\n");
        return 2;
    }
    if (mkdirs("resources/bin") != 0) return 1;
    FILE *sums = fopen("resources/bin/SHA256SUMS", "w");
    if (sums == NULL) return 1;
    int rc = 0;
    for (size_t i = 0; rc == 0 && i < COUNT(part_archs); ++i) {
        char out[128];
        snprintf(out, sizeof out, "resources/bin/rubrapack_ca-%s.dll", part_archs[i][0]);
        int n = snprintf(cmd, sizeof cmd,
                         "%s/bin/%s-w64-mingw32-clang -std=c23 -O2 -Wall -Wextra -Werror -shared -s -ffile-prefix-map=$PWD=. "
                         "-Wl,--no-insert-timestamp -Wl,--build-id=none src/ca/rubrapack_ca.c src/ca/rubrapack_ca.def "
                         "-lmsi -o %s",
                         root, part_archs[i][1], out);
        if (n < 0 || (size_t)n >= sizeof cmd || run(cmd) != 0) {
            rc = 1;
            break;
        }
        size_t len = 0;
        unsigned char *data = read_all(out, &len);
        if (data == NULL) {
            rc = 1;
            break;
        }
        char hex[65];
        sha256_hex(data, len, hex);
        free(data);
        fprintf(sums, "%s  rubrapack_ca-%s.dll\n", hex, part_archs[i][0]);
    }
    fclose(sums);
    if (rc == 0) printf("nob: built resources/bin (see SHA256SUMS)\n");
    return rc;
}

// build/<dir>/gen/ca_parts.c: each resources/bin/rubrapack_ca-<arch>.dll as a byte array.
static int gen_parts(const char *gen_dir, const char *gen_c) {
    if (mkdirs(gen_dir) != 0) return 1;
    FILE *f = fopen(gen_c, "w");
    if (f == NULL) return 1;
    fprintf(f, "// Generated by nob from resources/bin/rubrapack_ca-*.dll - do not edit.\n#include <stddef.h>\n");
    for (size_t i = 0; i < COUNT(part_archs); ++i) {
        char path[128];
        snprintf(path, sizeof path, "resources/bin/rubrapack_ca-%s.dll", part_archs[i][0]);
        size_t len = 0;
        unsigned char *data = read_all(path, &len);
        fprintf(f, "const unsigned char rp_ca_%s[] = {", part_archs[i][0]);
        for (size_t b = 0; data && b < len; ++b) fprintf(f, "%s%u", b == 0 ? "\n" : b % 32 ? "," : ",\n", data[b]);
        if (data == NULL || len == 0) fprintf(f, "0");
        fprintf(f, "};\nconst size_t rp_ca_%s_len = %zu;\n", part_archs[i][0], data ? len : (size_t)0);
        free(data);
    }
    fclose(f);
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

    char gen_dir[192], gen_c[256];
    snprintf(gen_dir, sizeof gen_dir, "%s/gen", dir);
    snprintf(gen_c, sizeof gen_c, "%s/ca_parts.c", gen_dir);
    if (gen_parts(gen_dir, gen_c) != 0) return 1;

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

    object_path(object, sizeof object, obj_dir, "gen/ca_parts.c");
    if (compile(t, "-std=c23 -w", gen_c, object) != 0) return 1;
    int gn = snprintf(link + link_len, sizeof link - link_len, " %s", object);
    if (gn < 0 || (size_t)gn >= sizeof link - link_len) return 1;
    link_len += (size_t)gn;

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
        if (strcmp(a, "parts") == 0) return parts();
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
                            "usage: ./nob [--target=native|win64] [--sanitize] [--debug] | parts | clean\n", a);
            return 2;
        }
    }
    return build(&t);
}
