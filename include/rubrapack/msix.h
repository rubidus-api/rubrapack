#ifndef RUBRAPACK_MSIX_H
#define RUBRAPACK_MSIX_H

// include/rubrapack/msix.h - MSIX packages (RFC-0009, P8a: one desktop full-trust application, one
// architecture, unsigned). The layout follows what Windows' packaging API writes
// (docs/research/2026-09-27-p8a-msix-oracle.md): the payload files, AppxManifest.xml,
// AppxBlockMap.xml (SHA-256 per 64 KiB block; deflated files one independent deflate part per
// block), [Content_Types].xml, in one ZIP64 archive (rubrapack/zip.h).

#include <stddef.h>
#include <stdint.h>

#include "proven/allocator.h"
#include "proven/types.h"
#include "rubrapack/ir.h"
#include "rubrapack/limits.h"
#include "rubrapack/srcdiag.h"

typedef struct {
    bool unsigned_test;     // --unsigned-test: the Publisher gets the unsigned-package OID (RFC-0001 13)
    bool store;             // --msix-compress store: no payload file is deflated
} rp_msix_options_t;

// Builds the package from the model. Problems are source diagnostics (RP16xx, RP1507...);
// PROVEN_ERR_INVALID_FORMAT when there were any.
[[nodiscard]] proven_err_t rp_msix_from_ir(proven_allocator_t alloc, const rp_ir_t *ir, const rp_msix_options_t *opt, uint8_t **out,
                                           size_t *len, rp_srcdiags_t *d);

// A payload file of a package, as its block map names it.
typedef struct {
    char    *name;          // block map name (backslashes), UTF-8
    uint64_t size;
    uint8_t *data;          // the file's bytes (checked against every block hash)
    bool     deflated;
} rp_msix_file_t;

// Opens a package and checks it the way Windows' reader does: the ZIP, the footprint files, the
// block map's hash method, every payload file present in both with the same size and local header
// size, and every block's SHA-256 (and, for deflated files, compressed size). *files lists the
// payload in block map order (free with rp_msix_files_free); *manifest gets AppxManifest.xml.
[[nodiscard]] proven_err_t rp_msix_open(proven_allocator_t alloc, const uint8_t *pkg, size_t len, const rp_limits_t *lim,
                                        rp_msix_file_t **files, size_t *count, uint8_t **manifest, size_t *manifest_len,
                                        const char **why);
void rp_msix_files_free(proven_allocator_t alloc, rp_msix_file_t *files, size_t count);

// The value of `attribute` on the first `element` (matched by its full name, prefix included) in
// an XML text, decoded; false when there is none or it does not fit.
[[nodiscard]] bool rp_xml_attr(const char *xml, size_t len, const char *element, const char *attribute, char *out, size_t cap);

// `rubrapack inspect|lint <file.msix>` (src/cli/msix.c).
[[nodiscard]] int rp_msix_inspect(const char *path, const char *what);
[[nodiscard]] int rp_msix_lint(const char *path, bool strict);

#endif // RUBRAPACK_MSIX_H
