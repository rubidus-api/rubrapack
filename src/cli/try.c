// src/cli/try.c - `rubrapack try <package> [-o <out.wsb>] [--host <folder>] [--network]` (RFC-0031
// B29): a Windows Sandbox configuration that installs the package in a fresh, disposable Windows.
// The package's folder is shared read-only on the sandbox's desktop; when the sandbox starts it
// runs msiexec (an MSI, with a log on the desktop) or Add-AppxPackage (an MSIX); closing the sandbox
// throws everything away. Networking is off unless --network. Open the .wsb on Windows with the
// Windows Sandbox feature turned on.

#include "rubrapack/cli.h"
#include "rubrapack/buf.h"
#include "rubrapack/diag.h"
#include "rubrapack/inspect.h"
#include "rubrapack/mem.h"
#include "rubrapack/pal.h"
#include "rubrapack/str.h"

#include <stdio.h>
#include <string.h>

#include "proven/heap.h"

#define SANDBOX_DESKTOP "C:\\Users\\WDAGUtilityAccount\\Desktop"

static void xml_escaped(rp_buf_t *b, const char *s) {
    for (; *s; ++s) {
        if (*s == '&') rp_buf_puts(b, "&amp;");
        else if (*s == '<') rp_buf_puts(b, "&lt;");
        else if (*s == '>') rp_buf_puts(b, "&gt;");
        else rp_buf_byte(b, (uint8_t)*s);
    }
}

int rp_cmd_try(int argc, char **argv) {
    const char *pkg = NULL, *out = NULL, *host = NULL;
    bool network = false;
    for (int i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) out = argv[++i];
        else if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) host = argv[++i];
        else if (strcmp(argv[i], "--network") == 0) network = true;
        else if (argv[i][0] != '-' && pkg == NULL) pkg = argv[i];
        else {
            (void)rp_cli_usage_error("try");
            return RP_EXIT_USAGE;
        }
    }
    bool msi = pkg && rp_ascii_ends_with_ci(pkg, ".msi");
    bool msix = pkg && (rp_ascii_ends_with_ci(pkg, ".msix") || rp_ascii_ends_with_ci(pkg, ".msixbundle"));
    if (!msi && !msix) {
        (void)rp_cli_usage_error("try");
        return RP_EXIT_USAGE;
    }
    proven_allocator_t heap = proven_heap_allocator();
    uint64_t size = 0;
    if (rp_pal_stat(heap, pkg, &size) != RP_FS_FILE) {
        rp_diag_error(RP_DIAG_INPUT, "cannot read '%s'", pkg);
        return RP_EXIT_IO;
    }
    // The package's name and folder: the folder is shared, the name runs inside.
    const char *slash = strrchr(pkg, '/');
#if defined(_WIN32)
    const char *bs = strrchr(pkg, '\\');
    if (bs && (!slash || bs > slash)) slash = bs;
#endif
    const char *name = slash ? slash + 1 : pkg;
    if (strpbrk(name, "\"'%")) {
        rp_diag_error(RP_DIAG_EXTRA_ARGUMENT, "the package's file name must not hold quotes or a percent sign: '%s'", name);
        return RP_EXIT_USAGE;
    }
    char dir[2048];
    snprintf(dir, sizeof dir, "%.*s", slash ? (int)(slash - pkg) : 1, slash ? pkg : ".");
    if (slash == pkg) snprintf(dir, sizeof dir, "/");
    char *full = host ? NULL : rp_pal_full_path(heap, dir);
    if (host == NULL && full == NULL) {
        rp_diag_error(RP_DIAG_INPUT, "cannot read '%s'", dir);
        return RP_EXIT_IO;
    }
    char wsb[2100];
    if (out == NULL) {
        const char *dot = strrchr(pkg, '.');
        snprintf(wsb, sizeof wsb, "%.*s.wsb", (int)(dot - pkg), pkg);
        out = wsb;
    }
    char stem[512];
    const char *ndot = strrchr(name, '.');
    snprintf(stem, sizeof stem, "%.*s", (int)(ndot - name), name);

    rp_buf_t b = rp_buf_new(heap, 1u << 20);
    rp_buf_puts(&b, "<Configuration>\r\n  <Networking>");
    rp_buf_puts(&b, network ? "Enable" : "Disable");
    rp_buf_puts(&b, "</Networking>\r\n  <MappedFolders>\r\n    <MappedFolder>\r\n      <HostFolder>");
    xml_escaped(&b, host ? host : full);
    rp_buf_puts(&b, "</HostFolder>\r\n      <SandboxFolder>" SANDBOX_DESKTOP "\\rubrapack-try</SandboxFolder>\r\n"
                    "      <ReadOnly>true</ReadOnly>\r\n    </MappedFolder>\r\n  </MappedFolders>\r\n  <LogonCommand>\r\n    <Command>");
    char cmd[3000];
    if (msi) {
        snprintf(cmd, sizeof cmd, "msiexec.exe /i \"" SANDBOX_DESKTOP "\\rubrapack-try\\%s\" /l*v \"" SANDBOX_DESKTOP "\\%s-install.log\"", name, stem);
    } else {
        snprintf(cmd, sizeof cmd, "powershell.exe -NoProfile -ExecutionPolicy Bypass -Command \"Add-AppxPackage -AllowUnsigned -Path '"
                 SANDBOX_DESKTOP "\\rubrapack-try\\%s'\"", name);
    }
    xml_escaped(&b, cmd);
    rp_buf_puts(&b, "</Command>\r\n  </LogonCommand>\r\n</Configuration>\r\n");
    rp_mem_free(heap, full);
    int rc = RP_EXIT_OK;
    if (b.err != PROVEN_OK || rp_pal_write_file_atomic(heap, out, b.data, b.len) != PROVEN_OK) {
        rp_diag_error(RP_DIAG_OUTPUT, "cannot write '%s'", out);
        rc = RP_EXIT_IO;
    }
    rp_buf_free(&b);
    return rc;
}
