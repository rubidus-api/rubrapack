// src/cli/new.c - `rubrapack new` and `rubrapack guid` (RFC-0006 5, RFC-0014).
//
//   new <file>.toml | new              asks for the answers one by one (stderr / stdin)
//   new <file>.toml --dist <dir> [...] the same answers as options, for scripts
//   new [msi] <name>                   a fixed starter source <name>.toml
// Sources are TOML: .toml by default; the older .rpk name is read and written as given.
//
// The source made from answers lists the top-level files of the program folder one by one (a
// shortcut needs a file ID, and a glob cannot leave a file out), each sub folder as a glob, and
// optional sub folders as features the user may tick (level 2).

#include "rubrapack/buf.h"
#include "rubrapack/diag.h"
#include "rubrapack/ident.h"
#include "rubrapack/inspect.h"
#include "rubrapack/mem.h"
#include "rubrapack/pal.h"
#include "rubrapack/pe.h"
#include "rubrapack/text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "proven/heap.h"

#include "new_int.h"

// snprintf into a small field whose value was checked before.
#define CUTX(out, cap, ...)                                                          \
    do {                                                                             \
        if (snprintf((out), (cap), __VA_ARGS__) >= (int)(cap)) (out)[(cap) - 1] = 0; \
    } while (0)

// A product name that is also a file name and a TOML string without escapes.
const char *rpn_name_problem(const char *s) {
    size_t n = strlen(s);
    if (n == 0 || n > 64) return "must be 1 to 64 bytes";
    if (rp_utf8_validate((const uint8_t *)s, n).err != PROVEN_OK) return "is not valid UTF-8";
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c == 0x7F || strchr("/\\:*?\"<>|$", c)) return "may not contain control characters or / \\ : * ? \" < > | $";
    }
    if (s[0] == ' ' || s[n - 1] == ' ' || s[n - 1] == '.') return "may not start with a space or end with a space or a dot";
    return NULL;
}

size_t rpn_source_ext(const char *s) {
    size_t n = strlen(s);
    if (n > 5 && rpn_ends_ci(s, ".toml")) return 5;
    if (n > 4 && rpn_ends_ci(s, ".rpk")) return 4;
    return 0;
}

// The summary information is ASCII (RFC-0001 9.10): an ASCII form of the name, when needed.
static void ascii_form(const char *name, char *out, size_t cap, bool *all_ascii) {
    size_t na = 0;
    *all_ascii = true;
    for (const char *s = name; *s && na + 1 < cap; ++s) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x80) out[na++] = (char)c;
        else *all_ascii = false;
    }
    while (na && out[na - 1] == ' ') --na;
    out[na] = '\0';
}

static int fixed_starter(const char *name, const char *ext) {
    char code[39];
    if (rp_uuid_random(code) != PROVEN_OK) {
        rp_diag_error(RP_DIAG_OUTPUT, "the system random source failed");
        return RP_EXIT_IO;
    }
    char ascii[65];
    bool all_ascii;
    ascii_form(name, ascii, sizeof ascii, &all_ascii);
    char summary[192] = "";
    if (!all_ascii) snprintf(summary, sizeof summary, "summary-name = \"%s\"         # ASCII: the file properties show this\n",
                             ascii[0] ? ascii : "package");
    char text[4096];
    int len = snprintf(text, sizeof text,
        "# %s%s - made by `rubrapack new`. Put the program's files in dist/ next to this file, then:\n"
        "#   rubrapack build \"%s%s\" -o \"%s.msi\"\n"
        "# Every table and key is described in the manual: https://rubidus-api.github.io/rubrapack/\n"
        "\n"
        "format = 1                         # the source format this rubrapack reads\n"
        "\n"
        "[package]\n"
        "name = \"%s\"\n"
        "%s"
        "manufacturer = \"%s authors\"     # shown in Settings > Installed apps\n"
        "version = \"1.0.0\"\n"
        "arch = \"x64\"                      # x64, arm64 or x86\n"
        "upgrade-code = \"%s\"   # keep it in every later version: it ties the upgrades together\n"
        "language = \"en-US\"                # or \"ko-KR\"\n"
        "\n"
        "[dir.INSTALLDIR]\n"
        "path = \"$(ProgramFiles)/%s\"\n"
        "\n"
        "[files.App]\n"
        "dir = \"INSTALLDIR\"\n"
        "glob = \"dist/*\"\n",
        name, ext, name, ext, name, name, summary, name, code, name);
    if (len < 0 || (size_t)len >= sizeof text) return RP_EXIT_IO;
    char path[160];
    snprintf(path, sizeof path, "%s%s", name, ext);
    proven_err_t err = rp_pal_write_file_new(proven_heap_allocator(), path, (const uint8_t *)text, (size_t)len);
    if (err == PROVEN_ERR_BUSY) {
        rp_diag_error(RP_DIAG_OUTPUT, "'%s' already exists; it is left as it is", path);
        return RP_EXIT_IO;
    }
    if (err != PROVEN_OK) {
        rp_diag_error(RP_DIAG_OUTPUT, "cannot write '%s'", path);
        return RP_EXIT_IO;
    }
    char line[200];
    snprintf(line, sizeof line, "wrote %s\n", path);
    return rp_pal_puts(RP_OUT_STDOUT, line) == PROVEN_OK ? RP_EXIT_OK : RP_EXIT_IO;
}

// ---- the program folder --------------------------------------------------------------------

int rpn_cmp_name(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

void rpn_names_free(names_t *x) {
    for (size_t i = 0; i < x->n; ++i) rp_mem_free(proven_heap_allocator(), x->v[i]);
    rp_mem_free(proven_heap_allocator(), x->v);
    *x = (names_t){ 0 };
}

char *rpn_join(const char *a, const char *b) {
    size_t na = strlen(a), nb = strlen(b);
    char *s = rp_mem_alloc(proven_heap_allocator(), na + nb + 2, 1);
    if (s == NULL) return NULL;
    memcpy(s, a, na);
    s[na] = '/';
    memcpy(s + na + 1, b, nb + 1);
    return s;
}

// A folder counts when some file lies below it (a glob without a match is an error).
static bool has_files(const char *dir, int depth) {
    proven_allocator_t heap = proven_heap_allocator();
    char **v = NULL;
    size_t n = 0;
    if (depth > 32 || rp_pal_list_dir(heap, dir, &v, &n) != PROVEN_OK) return false;
    bool found = false;
    for (size_t i = 0; i < n; ++i) {
        char *p = found ? NULL : rpn_join(dir, v[i]);
        uint64_t size = 0;
        rp_fskind_t k = p ? rp_pal_stat(heap, p, &size) : RP_FS_NONE;
        if (k == RP_FS_FILE || (k == RP_FS_DIR && has_files(p, depth + 1))) found = true;
        rp_mem_free(heap, p);
        rp_mem_free(heap, v[i]);
    }
    rp_mem_free(heap, v);
    return found;
}

proven_err_t rpn_scan_dist(const char *dist, scan_t *s) {
    proven_allocator_t heap = proven_heap_allocator();
    *s = (scan_t){ 0 };
    char **v = NULL;
    size_t n = 0;
    proven_err_t err = rp_pal_list_dir(heap, dist, &v, &n);
    if (err != PROVEN_OK) return err;
    s->files.v = rp_mem_alloc(heap, n + 1, sizeof(char *));
    s->dirs.v = rp_mem_alloc(heap, n + 1, sizeof(char *));
    if (s->files.v == NULL || s->dirs.v == NULL) err = PROVEN_ERR_NOMEM;
    for (size_t i = 0; i < n; ++i) {
        char *p = err == PROVEN_OK ? rpn_join(dist, v[i]) : NULL;
        uint64_t size = 0;
        rp_fskind_t k = p ? rp_pal_stat(heap, p, &size) : RP_FS_NONE;
        if (k == RP_FS_FILE) s->files.v[s->files.n++] = v[i];
        else if (k == RP_FS_DIR && has_files(p, 0)) s->dirs.v[s->dirs.n++] = v[i];
        else rp_mem_free(heap, v[i]);       // links are never followed (RFC-0002 8.1)
        rp_mem_free(heap, p);
    }
    rp_mem_free(heap, v);
    qsort(s->files.v, s->files.n, sizeof(char *), rpn_cmp_name);
    qsort(s->dirs.v, s->dirs.n, sizeof(char *), rpn_cmp_name);
    return err;
}

void rpn_scan_free(scan_t *s) {
    rpn_names_free(&s->files);
    rpn_names_free(&s->dirs);
}

bool rpn_in_names(const names_t *x, const char *s) {
    for (size_t i = 0; i < x->n; ++i) {
        if (strcmp(x->v[i], s) == 0) return true;
    }
    return false;
}

bool rpn_ends_ci(const char *s, const char *suffix) {
    size_t n = strlen(s), m = strlen(suffix);
    if (n < m) return false;
    for (size_t i = 0; i < m; ++i) {
        char a = s[n - m + i], b = suffix[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (a != b) return false;
    }
    return true;
}

// The architecture of a PE program, or NULL.
const char *rpn_pe_arch(const char *path) {
    proven_allocator_t heap = proven_heap_allocator();
    const uint8_t *data = NULL;
    size_t len = 0;
    rp_map_t *map = NULL;
    const char *arch = NULL;
    if (rp_pal_map_file(heap, path, (size_t)1 << 31, &data, &len, &map) == PROVEN_OK) {
        rp_pe_info_t pi;
        if (rp_pe_read(data, len, &pi) == PROVEN_OK && pi.is_pe) {
            arch = pi.machine == 0x8664 ? "x64" : pi.machine == 0x014C ? "x86" : pi.machine == 0xAA64 ? "arm64" : NULL;
        }
        rp_pal_unmap(heap, map);
    }
    return arch;
}

// A license next to the source or in the program folder: LICENSE.txt, .md or .rtf, any case.
void rpn_find_license(const char *dist, char *out, size_t cap) {
    proven_allocator_t heap = proven_heap_allocator();
    const char *where[] = { ".", dist };
    snprintf(out, cap, "-");
    for (int w = 0; w < 2; ++w) {
        names_t x = { 0 };
        if (rp_pal_list_dir(heap, where[w], &x.v, &x.n) != PROVEN_OK) continue;
        qsort(x.v, x.n, sizeof(char *), rpn_cmp_name);
        for (size_t i = 0; i < x.n && strcmp(out, "-") == 0; ++i) {
            const char *s = x.v[i];
            bool lic = (s[0] == 'L' || s[0] == 'l') && strlen(s) > 8 && (rpn_ends_ci(s, ".txt") || rpn_ends_ci(s, ".md") || rpn_ends_ci(s, ".rtf"));
            for (size_t k = 0; lic && k < 7; ++k) lic = (s[k] | 32) == "license"[k];
            if (lic && s[7] == '.') snprintf(out, cap, "%s%s%s", w ? dist : "", w ? "/" : "", s);
        }
        rpn_names_free(&x);
        if (strcmp(out, "-") != 0) return;
    }
}

// ---- answers ------------------------------------------------------------------------------

static const char *const archs[] = { "x64", "x86", "arm64", NULL };
static const char *const scopes[] = { "machine", "user", "dual", NULL };
static const char *const uis[] = { "none", "basic", "minimal", "installdir", "features", NULL };

static bool one_of(const char *s, const char *const *list) {
    for (size_t i = 0; list[i]; ++i) {
        if (strcmp(s, list[i]) == 0) return true;
    }
    return false;
}

const char *rpn_check_product(ans_t *a, char *v) {
    (void)a;
    size_t n = strlen(v);
    if (n == 0 || n > 128) return "give a name of 1 to 128 bytes";
    for (size_t i = 0; i < n; ++i) {
        if ((unsigned char)v[i] < 0x20 || v[i] == 0x7F) return "no control characters";
    }
    return NULL;
}

const char *rpn_check_version(ans_t *a, char *v) {
    (void)a;
    unsigned long p[5] = { 0 };
    int k = 0;
    const char *s = v;
    while (k < 5) {
        if (*s < '0' || *s > '9') return "a version is 3 or 4 numbers with dots, such as 1.0.0";
        char *end = NULL;
        p[k++] = strtoul(s, &end, 10);
        s = end;
        if (*s == '\0') break;
        if (*s != '.') return "a version is 3 or 4 numbers with dots, such as 1.0.0";
        ++s;
    }
    if (*s != '\0' || k < 3 || k > 4) return "a version is 3 or 4 numbers with dots, such as 1.0.0";
    if (p[0] > 255 || p[1] > 255 || p[2] > 65535 || p[3] > 65535) return "Windows Installer allows at most 255.255.65535(.65535)";
    return NULL;
}

const char *rpn_check_arch(ans_t *a, char *v) { (void)a; return one_of(v, archs) ? NULL : "x64, x86 or arm64"; }
const char *rpn_check_scope(ans_t *a, char *v) { (void)a; return one_of(v, scopes) ? NULL : "machine, user or dual"; }
const char *rpn_check_ui(ans_t *a, char *v) { (void)a; return one_of(v, uis) ? NULL : "none, basic, minimal, installdir or features"; }

const char *rpn_check_dist(ans_t *a, char *v) {
    for (char *p = v; *p; ++p) {
        if (*p == '\\') *p = '/';
    }
    size_t n = strlen(v);
    while (n > 1 && v[n - 1] == '/') v[--n] = '\0';
    if (n == 0 || v[0] == '/' || strchr(v, ':') || strstr(v, "..")) return "give a folder below this one, such as dist";
    uint64_t size = 0;
    if (rp_pal_stat(proven_heap_allocator(), v, &size) != RP_FS_DIR) return "there is no such folder here";
    if (a->scanned) rpn_scan_free(&a->scan);
    a->scanned = rpn_scan_dist(v, &a->scan) == PROVEN_OK;
    if (!a->scanned) return "cannot read that folder";
    if (a->scan.files.n == 0 && a->scan.dirs.n == 0) return "that folder holds no files";
    return NULL;
}

const char *rpn_check_main(ans_t *a, char *v) {
    if (strcmp(v, "-") == 0 || rpn_in_names(&a->scan.files, v)) return NULL;
    return "give a file directly in the program folder, or - for none";
}

const char *rpn_check_folder(ans_t *a, char *v) {
    (void)a;
    const char *why = rpn_name_problem(v);
    return why ? "a folder name without / \\ : * ? \" < > | $" : NULL;
}

const char *rpn_check_optional(ans_t *a, char *v) {
    if (strcmp(v, "-") == 0) return NULL;
    char copy[1024];
    snprintf(copy, sizeof copy, "%s", v);
    size_t count = 0;
    for (char *t = strtok(copy, ","); t; t = strtok(NULL, ",")) {
        if (!rpn_in_names(&a->scan.dirs, t)) return "give sub folders of the program folder, separated by commas, or - for none";
        ++count;
    }
    return count ? NULL : "give sub folders of the program folder, separated by commas, or - for none";
}

const char *rpn_check_license(ans_t *a, char *v) {
    (void)a;
    for (char *p = v; *p; ++p) {
        if (*p == '\\') *p = '/';
    }
    if (strcmp(v, "-") == 0) return NULL;
    uint64_t size = 0;
    if (!(rpn_ends_ci(v, ".txt") || rpn_ends_ci(v, ".md") || rpn_ends_ci(v, ".rtf"))) return "a .txt, .md or .rtf file, or - for none";
    if (v[0] == '/' || strchr(v, ':') || strstr(v, "..")) return "give a file below this folder";
    if (rp_pal_stat(proven_heap_allocator(), v, &size) != RP_FS_FILE) return "there is no such file here";
    return NULL;
}

const char *rpn_check_languages(ans_t *a, char *v) { (void)a; return strcmp(v, "-") == 0 || strcmp(v, "ko") == 0 ? NULL : "ko, or - for English only"; }

static const char *check_shortcuts(ans_t *a, char *v) {
    if (strcmp(v, "none") == 0) return NULL;
    if (strcmp(a->main, "-") == 0) return "no main program, so no shortcuts: none";
    return strcmp(v, "start") == 0 || strcmp(v, "desktop") == 0 || strcmp(v, "start,desktop") == 0 || strcmp(v, "desktop,start") == 0
               ? NULL
               : "start, desktop, start,desktop or none";
}

static const char *check_stem(ans_t *a, char *v) {
    (void)a;
    size_t n = strlen(v), e = rpn_source_ext(v);
    if (e) {                            // the name given keeps its extension
        CUTX(a->ext, sizeof a->ext, "%s", v + n - e);
        v[n - e] = '\0';
    }
    if (rpn_name_problem(v)) return "a file name without / \\ : * ? \" < > | $";
    char path[160];
    snprintf(path, sizeof path, "%s%s", v, a->ext);
    uint64_t size = 0;
    if (rp_pal_stat(proven_heap_allocator(), path, &size) != RP_FS_NONE) return "that file exists already; give another name";
    return NULL;
}

// One question on stderr, the answer from stdin; Enter takes the default. 0 = answered.
int rpn_ask(ans_t *a, const char *question, const char *dflt, char *out, size_t cap, const char *(*check)(ans_t *, char *)) {
    proven_allocator_t heap = proven_heap_allocator();
    for (;;) {
        char prompt[1400];
        snprintf(prompt, sizeof prompt, "%s%s%s%s: ", question, dflt[0] ? " [" : "", dflt, dflt[0] ? "]" : "");
        if (rp_pal_puts(RP_OUT_STDERR, prompt) != PROVEN_OK) return RP_EXIT_IO;
        char *line = NULL;
        proven_err_t err = rp_pal_read_line(heap, 4096, &line);
        if (err == PROVEN_ERR_NOT_FOUND) {
            (void)rp_pal_puts(RP_OUT_STDERR, "\n");
            rp_diag_error(RP_DIAG_INPUT, "the input ended before every question was answered; nothing was written");
            return RP_EXIT_USAGE;
        }
        if (err != PROVEN_OK) {
            (void)rp_pal_puts(RP_OUT_STDERR, "  that was not readable text; again, please\n");
            continue;
        }
        char *s = line;
        while (*s == ' ' || *s == '\t') ++s;
        size_t n = strlen(s);
        while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = '\0';
        if (n == 0) s = (char *)dflt;
        if (strlen(s) >= cap) {
            rp_mem_free(heap, line);
            (void)rp_pal_puts(RP_OUT_STDERR, "  too long; again, please\n");
            continue;
        }
        snprintf(out, cap, "%s", s);
        rp_mem_free(heap, line);
        const char *why = check ? check(a, out) : NULL;
        if (why == NULL) return 0;
        char msg[300];
        snprintf(msg, sizeof msg, "  %s\n", why);
        (void)rp_pal_puts(RP_OUT_STDERR, msg);
    }
}

int rpn_ask_yes(ans_t *a, const char *question, bool dflt, bool *yes) {
    char v[8];
    for (;;) {
        int rc = rpn_ask(a, question, dflt ? "Y/n" : "y/N", v, sizeof v, NULL);
        if (rc) return rc;
        if (strcmp(v, "Y/n") == 0 || strcmp(v, "y/N") == 0) {
            *yes = dflt;
            return 0;
        }
        if (v[0] == 'y' || v[0] == 'Y') { *yes = true; return 0; }
        if (v[0] == 'n' || v[0] == 'N') { *yes = false; return 0; }
        (void)rp_pal_puts(RP_OUT_STDERR, "  y or n\n");
    }
}

// Defaults that depend on earlier answers.
static void default_main(ans_t *a, char *out, size_t cap) {
    snprintf(out, cap, "-");
    for (size_t i = 0; i < a->scan.files.n; ++i) {
        if (rpn_ends_ci(a->scan.files.v[i], ".exe")) {
            snprintf(out, cap, "%s", a->scan.files.v[i]);
            return;
        }
    }
}

static void default_arch(ans_t *a, char *out, size_t cap) {
    const char *arch = NULL;
    if (strcmp(a->main, "-") != 0) {
        char *p = rpn_join(a->dist, a->main);
        if (p) arch = rpn_pe_arch(p);
        rp_mem_free(proven_heap_allocator(), p);
    }
    snprintf(out, cap, "%s", arch ? arch : "x64");
}

static void default_stem(const ans_t *a, char *out, size_t cap) {
    snprintf(out, cap, "%s%s", rpn_name_problem(a->name) ? "app" : a->name, a->ext);
}

static int interview(ans_t *a, const char *stem) {
    int rc;
    (void)rp_pal_puts(RP_OUT_STDERR, "rubrapack new: a few questions make the source (Enter takes the value in [ ]).\n");
    if ((rc = rpn_ask(a, "Product name", stem ? stem : "", a->name, sizeof a->name, rpn_check_product))) return rc;
    char d[512];
    snprintf(d, sizeof d, "%s authors", a->name);
    if ((rc = rpn_ask(a, "Manufacturer (shown in Installed apps)", d, a->manufacturer, sizeof a->manufacturer, rpn_check_product))) return rc;
    if ((rc = rpn_ask(a, "Version", "1.0.0", a->version, sizeof a->version, rpn_check_version))) return rc;
    if ((rc = rpn_ask(a, "Folder with the files to install", "dist", a->dist, sizeof a->dist, rpn_check_dist))) return rc;
    char line[1400];
    snprintf(line, sizeof line, "  %zu files and %zu folders there\n", a->scan.files.n, a->scan.dirs.n);
    (void)rp_pal_puts(RP_OUT_STDERR, line);
    default_main(a, d, sizeof d);
    if ((rc = rpn_ask(a, "Main program, for shortcuts (- for none)", d, a->main, sizeof a->main, rpn_check_main))) return rc;
    default_arch(a, d, sizeof d);
    if ((rc = rpn_ask(a, "Architecture (x64, x86, arm64)", d, a->arch, sizeof a->arch, rpn_check_arch))) return rc;
    if ((rc = rpn_ask(a, "Folder name under Program Files", rpn_name_problem(a->name) ? "" : a->name, a->install_dir, sizeof a->install_dir,
                  rpn_check_folder))) return rc;
    if ((rc = rpn_ask(a, "Install for (machine, user, dual)", "machine", a->scope, sizeof a->scope, rpn_check_scope))) return rc;
    snprintf(a->optional, sizeof a->optional, "-");
    if (a->scan.dirs.n) {
        size_t o = (size_t)snprintf(line, sizeof line, "  sub folders:");
        for (size_t i = 0; i < a->scan.dirs.n && o < sizeof line - 1; ++i) o += (size_t)snprintf(line + o, sizeof line - o, " %s", a->scan.dirs.v[i]);
        if (o < sizeof line - 1) snprintf(line + o, sizeof line - o, "\n");
        (void)rp_pal_puts(RP_OUT_STDERR, line);
        if ((rc = rpn_ask(a, "Optional parts the user may tick (sub folders, commas; - for none)", "-", a->optional, sizeof a->optional,
                      rpn_check_optional))) return rc;
    }
    if ((rc = rpn_ask(a, "Dialogs (none, basic, minimal, installdir, features)", strcmp(a->optional, "-") ? "features" : "installdir", a->ui,
                  sizeof a->ui, rpn_check_ui))) return rc;
    snprintf(a->license, sizeof a->license, "-");
    if (strcmp(a->ui, "none") != 0 && strcmp(a->ui, "basic") != 0) {
        rpn_find_license(a->dist, d, sizeof d);
        if ((rc = rpn_ask(a, "License to accept (.txt, .md, .rtf; - for none)", d, a->license, sizeof a->license, rpn_check_license))) return rc;
    }
    snprintf(a->languages, sizeof a->languages, "-");
    if (strcmp(a->ui, "none") != 0) {
        bool ko = false;
        if ((rc = rpn_ask_yes(a, "Korean dialogs as well as English?", false, &ko))) return rc;
        if (ko) snprintf(a->languages, sizeof a->languages, "ko");
    }
    snprintf(a->shortcuts, sizeof a->shortcuts, "none");
    if (strcmp(a->main, "-") != 0) {
        bool start = true, desk = false;
        if ((rc = rpn_ask_yes(a, "Start menu shortcut?", true, &start))) return rc;
        if ((rc = rpn_ask_yes(a, "Desktop shortcut?", false, &desk))) return rc;
        snprintf(a->shortcuts, sizeof a->shortcuts, "%s", start && desk ? "start,desktop" : start ? "start" : desk ? "desktop" : "none");
    }
    if (stem) {
        snprintf(a->stem, sizeof a->stem, "%s", stem);
        if (check_stem(a, a->stem) == NULL) return 0;
    }
    default_stem(a, d, sizeof d);
    return rpn_ask(a, "Source file to write", d, a->stem, sizeof a->stem, check_stem);
}

// ---- the source ---------------------------------------------------------------------------

bool rpn_id_used(const ids_t *ids, const char *s) {
    for (size_t i = 0; i < ids->n; ++i) {
        const char *a = ids->v[i], *b = s;
        while (*a && *b && ((*a | 32) == (*b | 32))) ++a, ++b;
        if (*a == '\0' && *b == '\0') return true;
    }
    return false;
}

// An ID from a file or folder name: letters, digits and '_' (IDs are shared by every table).
const char *rpn_make_id(ids_t *ids, const char *name, const char *suffix, char *out, size_t cap) {
    char base[40];
    size_t n = 0;
    for (const char *s = name; *s && n < 30; ++s) {
        char c = *s;
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        base[n++] = ok ? c : '_';
    }
    base[n] = '\0';
    bool letters = false;
    for (size_t i = 0; i < n; ++i) letters |= base[i] != '_';
    if (!letters) snprintf(base, sizeof base, "Item");
    for (int k = 1; k < 10000; ++k) {
        char num[12] = "";
        if (k > 1) snprintf(num, sizeof num, "_%d", k);
        snprintf(out, cap, "%s%s%s%s", base[0] >= '0' && base[0] <= '9' ? "_" : "", base, suffix, num);
        if (!rpn_id_used(ids, out)) break;
    }
    if (ids->n < sizeof ids->v / sizeof ids->v[0]) snprintf(ids->v[ids->n++], sizeof ids->v[0], "%s", out);
    return out;
}

void rpn_toml_str(rp_buf_t *b, const char *s) {
    rp_buf_byte(b, '"');
    for (; *s; ++s) {
        if (*s == '"' || *s == '\\') rp_buf_byte(b, '\\');
        rp_buf_byte(b, (uint8_t)*s);
    }
    rp_buf_byte(b, '"');
}

static void kv(rp_buf_t *b, const char *key, const char *value, const char *comment) {
    rp_buf_puts(b, key);
    rp_buf_puts(b, " = ");
    rpn_toml_str(b, value);
    if (comment) {
        rp_buf_puts(b, "   # ");
        rp_buf_puts(b, comment);
    }
    rp_buf_byte(b, '\n');
}

static bool listed(const char *list, const char *item) {
    size_t n = strlen(item);
    for (const char *p = list; (p = strstr(p, item)) != NULL; p += n) {
        if ((p == list || p[-1] == ',') && (p[n] == '\0' || p[n] == ',')) return true;
    }
    return false;
}

static proven_err_t make_source(const ans_t *a, uint8_t **out, size_t *len) {
    char code[39];
    proven_err_t err = rp_uuid_random(code);
    if (err != PROVEN_OK) return err;
    rp_buf_t b = rp_buf_new(proven_heap_allocator(), (size_t)1 << 22);
    char line[1200];
    snprintf(line, sizeof line,
             "# %s%s - made by `rubrapack new`. Build it, and the next version, with:\n"
             "#   rubrapack build \"%s%s\" -o \"%s.msi\"\n"
             "#   rubrapack build \"%s%s\" -o \"%s-1.0.1.msi\" -D VERSION=1.0.1\n"
             "# Every table and key is described in the manual: https://rubidus-api.github.io/rubrapack/\n\n",
             a->stem, a->ext, a->stem, a->ext, a->stem, a->stem, a->ext, a->stem);
    rp_buf_puts(&b, line);
    rp_buf_puts(&b, "format = 1                       # the source format this rubrapack reads\n\n[package]\n");
    kv(&b, "name", a->name, NULL);
    char ascii[129];
    bool all_ascii;
    ascii_form(a->name, ascii, sizeof ascii, &all_ascii);
    if (!all_ascii) kv(&b, "summary-name", ascii[0] ? ascii : "package", "ASCII: the file properties show this");
    kv(&b, "manufacturer", a->manufacturer, "shown in Settings > Installed apps");
    kv(&b, "version", "$(VERSION)", "set in [define] below, or -D VERSION=... when building");
    kv(&b, "arch", a->arch, "x64, arm64 or x86");
    kv(&b, "upgrade-code", code, "keep it in every later version: it ties the upgrades together");
    if (strcmp(a->scope, "machine") != 0) kv(&b, "scope", a->scope, strcmp(a->scope, "user") == 0 ? "for the current user, no administrator" : "per user, or for everyone from an elevated prompt");
    if (strcmp(a->ui, "none") != 0) kv(&b, "ui", a->ui, NULL);
    if (strcmp(a->license, "-") != 0) kv(&b, "license", a->license, "Next stays off until it is accepted");
    rp_buf_puts(&b, "\n[define]\n");
    kv(&b, "VERSION", a->version, NULL);
    if (strcmp(a->languages, "-") != 0) {
        rp_buf_puts(&b, "\n[ui]\nlanguages = [\"ko\"]   # a language page first; Korean is chosen on Korean systems\n");
    }
    static ids_t ids;               // static: large; `new` makes one source per run
    ids.n = 0;
    char tmp[64];
    static const char *const fixed[] = { "INSTALLDIR", "Main", "StartMenu", "Desktop", "VERSION" };
    for (size_t i = 0; i < sizeof fixed / sizeof fixed[0]; ++i) snprintf(ids.v[ids.n++], sizeof ids.v[0], "%s", fixed[i]);
    bool optional = strcmp(a->optional, "-") != 0;
    if (optional) {
        rp_buf_puts(&b, "\n[feature.Main]\n");
        kv(&b, "title", a->name, NULL);
        rp_buf_puts(&b, "required = true                  # always installed\n");
    }
    // Feature IDs for the optional folders first, so the dirs and globs can name them.
    static char feat[4096][48];
    memset(feat, 0, sizeof feat);
    for (size_t i = 0; optional && i < a->scan.dirs.n && i < 4096; ++i) {
        if (!listed(a->optional, a->scan.dirs.v[i])) continue;
        rpn_make_id(&ids, a->scan.dirs.v[i], "", feat[i], sizeof feat[i]);
        rp_buf_puts(&b, "\n[feature.");
        rp_buf_puts(&b, feat[i]);
        rp_buf_puts(&b, "]\n");
        kv(&b, "title", a->scan.dirs.v[i], NULL);
        rp_buf_puts(&b, "level = 2                        # offered, not ticked by default\n");
    }
    rp_buf_puts(&b, "\n[dir.INSTALLDIR]\n");
    snprintf(line, sizeof line, "$(ProgramFiles)/%s", a->install_dir);
    kv(&b, "path", line, strcmp(a->scope, "machine") == 0 ? NULL : "under %LOCALAPPDATA%\\Programs when installed per user");
    if (optional) kv(&b, "feature", "Main", NULL);
    char main_id[64] = "";
    for (size_t i = 0; i < a->scan.files.n; ++i) {
        const char *f = a->scan.files.v[i];
        rpn_make_id(&ids, f, "", tmp, sizeof tmp);
        if (strcmp(f, a->main) == 0) snprintf(main_id, sizeof main_id, "%s", tmp);
        snprintf(line, sizeof line, "\n[file.%s]\n", tmp);
        rp_buf_puts(&b, line);
        kv(&b, "dir", "INSTALLDIR", NULL);
        snprintf(line, sizeof line, "%s/%s", a->dist, f);
        kv(&b, "source", line, NULL);
    }
    for (size_t i = 0; i < a->scan.dirs.n; ++i) {
        const char *f = a->scan.dirs.v[i];
        char dir_id[64], files_id[64];
        rpn_make_id(&ids, f, "_dir", dir_id, sizeof dir_id);
        rpn_make_id(&ids, f, "_files", files_id, sizeof files_id);
        snprintf(line, sizeof line, "\n[dir.%s]\n", dir_id);
        rp_buf_puts(&b, line);
        snprintf(line, sizeof line, "$(INSTALLDIR)/%s", f);
        kv(&b, "path", line, NULL);
        if (i < 4096 && feat[i][0]) kv(&b, "feature", feat[i], NULL);
        else if (optional) kv(&b, "feature", "Main", NULL);      // a dir does not take its parent's feature
        snprintf(line, sizeof line, "\n[files.%s]\n", files_id);
        rp_buf_puts(&b, line);
        kv(&b, "dir", dir_id, NULL);
        snprintf(line, sizeof line, "%s/%s/**", a->dist, f);
        kv(&b, "glob", line, NULL);
    }
    if (main_id[0] && listed(a->shortcuts, "start")) {
        rp_buf_puts(&b, "\n[shortcut.StartMenu]\ndir = \"Programs\"\n");
        kv(&b, "name", a->name, NULL);
        snprintf(line, sizeof line, "file:%s", main_id);
        kv(&b, "target", line, NULL);
    }
    if (main_id[0] && listed(a->shortcuts, "desktop")) {
        rp_buf_puts(&b, "\n[shortcut.Desktop]\ndir = \"Desktop\"\n");
        kv(&b, "name", a->name, NULL);
        snprintf(line, sizeof line, "file:%s", main_id);
        kv(&b, "target", line, NULL);
    }
    return rp_buf_take(&b, out, len);
}

// The same answers as one command, to repeat without questions.
static void print_command(const ans_t *a) {
    const char *keys[] = { "--name", "--manufacturer", "--version", "--arch", "--dist", "--main", "--install-dir", "--scope",
                           "--ui", "--license", "--languages", "--optional", "--shortcuts" };
    const char *vals[] = { a->name, a->manufacturer, a->version, a->arch, a->dist, a->main, a->install_dir, a->scope,
                           a->ui, a->license, a->languages, a->optional, a->shortcuts };
    rp_buf_t b = rp_buf_new(proven_heap_allocator(), 1 << 16);
    rp_buf_puts(&b, "the same without questions:\n  rubrapack new ");
    char file[140];
    snprintf(file, sizeof file, "%s%s", a->stem, a->ext);
    rpn_toml_str(&b, file);
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; ++i) {
        rp_buf_byte(&b, ' ');
        rp_buf_puts(&b, keys[i]);
        rp_buf_byte(&b, ' ');
        rpn_toml_str(&b, vals[i]);
    }
    rp_buf_byte(&b, '\n');
    uint8_t *text = NULL;
    size_t len = 0;
    if (rp_buf_take(&b, &text, &len) == PROVEN_OK) (void)rp_pal_write(RP_OUT_STDOUT, text, len);
    rp_mem_free(proven_heap_allocator(), text);
}

static int usage(void) {
    rp_diag_error(RP_DIAG_EXTRA_ARGUMENT,
                  "usage: rubrapack new [<file>.toml] | rubrapack new <file>.toml --dist <folder> "
                  "[--name <text>] [--manufacturer <text>] [--version <a.b.c>] [--arch x64|x86|arm64] [--main <file>|-] "
                  "[--install-dir <folder>] [--scope machine|user|dual] [--ui none|basic|minimal|installdir|features] "
                  "[--license <file>|-] [--languages ko|-] [--optional <folder,...>|-] [--shortcuts start,desktop|none] | "
                  "rubrapack new <name> (a fixed starter <name>.toml); .rpk names are taken too");
    return RP_EXIT_USAGE;
}

int rp_cmd_new(int argc, char **argv) {
    const char *kind = "msi", *name = NULL;
    bool interactive = false, options = false;
    ans_t *a = rp_mem_alloc(proven_heap_allocator(), 1, sizeof *a);
    if (a == NULL) return RP_EXIT_IO;
    memset(a, 0, sizeof *a);
    struct { const char *key; char *field; size_t cap; } opts[] = {
        { "--name", a->name, sizeof a->name }, { "--manufacturer", a->manufacturer, sizeof a->manufacturer },
        { "--version", a->version, sizeof a->version }, { "--arch", a->arch, sizeof a->arch },
        { "--dist", a->dist, sizeof a->dist }, { "--main", a->main, sizeof a->main },
        { "--install-dir", a->install_dir, sizeof a->install_dir }, { "--scope", a->scope, sizeof a->scope },
        { "--ui", a->ui, sizeof a->ui }, { "--license", a->license, sizeof a->license },
        { "--languages", a->languages, sizeof a->languages }, { "--optional", a->optional, sizeof a->optional },
        { "--shortcuts", a->shortcuts, sizeof a->shortcuts },
    };
    const char *pos[2] = { NULL, NULL };
    size_t npos = 0;
    int rc = RP_EXIT_OK;
    for (int i = 2; i < argc && rc == RP_EXIT_OK; ++i) {
        const char *s = argv[i];
        if (strcmp(s, "-i") == 0 || strcmp(s, "--interactive") == 0) {
            interactive = true;
            continue;
        }
        bool known = false;
        for (size_t k = 0; k < sizeof opts / sizeof opts[0]; ++k) {
            if (strcmp(s, opts[k].key) != 0) continue;
            known = true;
            if (i + 1 >= argc || strlen(argv[i + 1]) >= opts[k].cap) rc = usage();
            else snprintf(opts[k].field, opts[k].cap, "%s", argv[++i]);
            options = true;
        }
        if (known) continue;
        if (s[0] == '-' || npos == 2) rc = usage();
        else pos[npos++] = s;
    }
    if (rc == RP_EXIT_OK && npos == 2) {
        kind = pos[0];
        name = pos[1];
    } else if (npos == 1) {
        name = pos[0];
    }
    // `new app.toml` (or app.rpk) names the file to write and asks for the rest, as `edit` names it.
    char stem_buf[128];
    bool file_named = false;
    snprintf(a->ext, sizeof a->ext, ".toml");
    size_t e = name ? rpn_source_ext(name) : 0;
    if (rc == RP_EXIT_OK && e && strlen(name) < sizeof stem_buf) {
        snprintf(a->ext, sizeof a->ext, "%s", name + strlen(name) - e);
        snprintf(stem_buf, sizeof stem_buf, "%.*s", (int)(strlen(name) - e), name);
        name = stem_buf;
        file_named = true;
        uint64_t size = 0;
        char path[160];
        snprintf(path, sizeof path, "%s%s", name, a->ext);
        if (!rpn_name_problem(name) && rp_pal_stat(proven_heap_allocator(), path, &size) != RP_FS_NONE) {
            rp_diag_error(RP_DIAG_OUTPUT, "'%s' exists already; change it with `rubrapack edit %s`", path, path);
            rc = RP_EXIT_IO;
        }
    }
    if (rc == RP_EXIT_OK && strcmp(kind, "msix") == 0) {
        rp_diag_error(RP_DIAG_NOT_IMPLEMENTED, "new msix is not implemented yet: make an MSI source and add [msix] (see the manual)");
        rc = RP_EXIT_USAGE;
    } else if (rc == RP_EXIT_OK && strcmp(kind, "msi") != 0) {
        rc = usage();
    }
    if (rc == RP_EXIT_OK && name && rpn_name_problem(name)) {
        rp_diag_error(RP_DIAG_BAD_ARG_TEXT, "the name %s", rpn_name_problem(name));
        rc = RP_EXIT_USAGE;
    }
    if (rc == RP_EXIT_OK && interactive && options) {
        rp_diag_error(RP_DIAG_EXTRA_ARGUMENT, "-i asks for every answer; give either -i or the options");
        rc = RP_EXIT_USAGE;
    }
    if (rc == RP_EXIT_OK && !interactive && !options) {
        if (name && !file_named) {                     // `new app`: the fixed starter, as before
            rp_mem_free(proven_heap_allocator(), a);
            return fixed_starter(name, ".toml");
        }
        interactive = true;                             // `new` or `new app.toml` asks
    }
    if (rc == RP_EXIT_OK && interactive) {
        rc = interview(a, name);
    } else if (rc == RP_EXIT_OK) {
        // Options: the defaults the questions would offer, then every answer checked.
        if (name == NULL) rc = usage();
        struct { char *field; size_t cap; const char *(*check)(ans_t *, char *); const char *what; } chk[] = {
            { a->name, sizeof a->name, rpn_check_product, "--name" }, { a->manufacturer, sizeof a->manufacturer, rpn_check_product, "--manufacturer" },
            { a->version, sizeof a->version, rpn_check_version, "--version" }, { a->dist, sizeof a->dist, rpn_check_dist, "--dist" },
            { a->main, sizeof a->main, rpn_check_main, "--main" }, { a->arch, sizeof a->arch, rpn_check_arch, "--arch" },
            { a->install_dir, sizeof a->install_dir, rpn_check_folder, "--install-dir" }, { a->scope, sizeof a->scope, rpn_check_scope, "--scope" },
            { a->optional, sizeof a->optional, rpn_check_optional, "--optional" }, { a->ui, sizeof a->ui, rpn_check_ui, "--ui" },
            { a->license, sizeof a->license, rpn_check_license, "--license" }, { a->languages, sizeof a->languages, rpn_check_languages, "--languages" },
            { a->shortcuts, sizeof a->shortcuts, check_shortcuts, "--shortcuts" }, { a->stem, sizeof a->stem, check_stem, "the source file" },
        };
        if (rc == RP_EXIT_OK) snprintf(a->stem, sizeof a->stem, "%s", name);
        for (size_t k = 0; rc == RP_EXIT_OK && k < sizeof chk / sizeof chk[0]; ++k) {
            char *f = chk[k].field;
            if (f[0] == '\0') {             // the default, from the answers before it
                if (f == a->name) snprintf(f, chk[k].cap, "%s", name);
                else if (f == a->manufacturer) snprintf(f, chk[k].cap, "%s authors", a->name);
                else if (f == a->version) snprintf(f, chk[k].cap, "1.0.0");
                else if (f == a->dist) snprintf(f, chk[k].cap, "dist");
                else if (f == a->main) default_main(a, f, chk[k].cap);
                else if (f == a->arch) default_arch(a, f, chk[k].cap);
                else if (f == a->install_dir) snprintf(f, chk[k].cap, "%s", rpn_name_problem(a->name) ? name : a->name);
                else if (f == a->scope) snprintf(f, chk[k].cap, "machine");
                else if (f == a->optional) snprintf(f, chk[k].cap, "-");
                else if (f == a->ui) snprintf(f, chk[k].cap, "%s", strcmp(a->optional, "-") ? "features" : "installdir");
                else if (f == a->license) {
                    if (strcmp(a->ui, "none") && strcmp(a->ui, "basic")) rpn_find_license(a->dist, f, chk[k].cap);
                    else snprintf(f, chk[k].cap, "-");
                } else if (f == a->languages) snprintf(f, chk[k].cap, "-");
                else if (f == a->shortcuts) snprintf(f, chk[k].cap, "%s", strcmp(a->main, "-") ? "start" : "none");
            }
            const char *why = chk[k].check(a, f);
            if (why) {
                rp_diag_error(RP_DIAG_BAD_ARG_TEXT, "%s '%s': %s", chk[k].what, f, why);
                rc = RP_EXIT_USAGE;
            }
        }
        if (rc == RP_EXIT_OK && strcmp(a->languages, "-") != 0 && strcmp(a->ui, "none") == 0) {
            rp_diag_error(RP_DIAG_EXTRA_ARGUMENT, "--languages needs dialogs: give --ui too");
            rc = RP_EXIT_USAGE;
        }
    }
    uint8_t *text = NULL;
    size_t len = 0;
    char path[160];
    snprintf(path, sizeof path, "%s%s", a->stem, a->ext);
    if (rc == RP_EXIT_OK && make_source(a, &text, &len) != PROVEN_OK) rc = RP_EXIT_IO;
    if (rc == RP_EXIT_OK) {
        proven_err_t err = rp_pal_write_file_new(proven_heap_allocator(), path, text, len);
        if (err != PROVEN_OK) {
            rp_diag_error(RP_DIAG_OUTPUT, err == PROVEN_ERR_BUSY ? "'%s' already exists; it is left as it is" : "cannot write '%s'", path);
            rc = RP_EXIT_IO;
        }
    }
    rp_mem_free(proven_heap_allocator(), text);
    if (rc == RP_EXIT_OK) {
        char line[240];
        snprintf(line, sizeof line, "wrote %s\n", path);
        if (rp_pal_puts(RP_OUT_STDOUT, line) != PROVEN_OK) rc = RP_EXIT_IO;
        if (interactive) print_command(a);
        char *lint_argv[] = { argv[0], "lint", path, NULL };
        int lrc = rp_cmd_lint_source(3, lint_argv);
        (void)rp_pal_puts(RP_OUT_STDOUT, lrc == RP_EXIT_OK ? "lint: no problems\n" : "lint: see the problems above\n");
        if (rc == RP_EXIT_OK) rc = lrc;
    }
    if (a->scanned) rpn_scan_free(&a->scan);
    rp_mem_free(proven_heap_allocator(), a);
    return rc;
}

// `guid`: a random UUIDv4. `guid --from <text>`: the UUIDv8 rubrapack derives (include/rubrapack/
// ident.h) with the tag "guid" and the text as its one field (RFC-0006 L5).
int rp_cmd_guid(int argc, char **argv) {
    char out[39];
    if (argc == 2) {
        if (rp_uuid_random(out) != PROVEN_OK) {
            rp_diag_error(RP_DIAG_OUTPUT, "the system random source failed");
            return RP_EXIT_IO;
        }
    } else if (argc == 4 && strcmp(argv[2], "--from") == 0) {
        const char *fields[1] = { argv[3] };
        rp_uuid_derive("guid", fields, 1, out);
    } else {
        rp_diag_error(RP_DIAG_EXTRA_ARGUMENT, "usage: rubrapack guid [--from <text>]");
        return RP_EXIT_USAGE;
    }
    char line[48];
    snprintf(line, sizeof line, "%s\n", out);
    return rp_pal_puts(RP_OUT_STDOUT, line) == PROVEN_OK ? RP_EXIT_OK : RP_EXIT_IO;
}
