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
#include "rubrapack/sign.h"
#include "rubrapack/srcdiag.h"

typedef struct {
    bool unsigned_test;     // --unsigned-test: the Publisher gets the unsigned-package OID (RFC-0001 13)
    bool store;             // --msix-compress store: no payload file is deflated
} rp_msix_options_t;

// Builds the package from the model. Problems are source diagnostics (RP16xx, RP1507...);
// PROVEN_ERR_INVALID_FORMAT when there were any.
[[nodiscard]] proven_err_t rp_msix_from_ir(proven_allocator_t alloc, const rp_ir_t *ir, const rp_msix_options_t *opt, uint8_t **out,
                                           size_t *len, rp_srcdiags_t *d);

// The App Installer file (RFC-0016 3) for a package or, with bundle set, a bundle built from this
// model: where it and the package are downloaded from ([msix] appinstaller-uri, package-uri) and
// how often Windows looks for a newer version. NULL output when the source asks for none.
[[nodiscard]] proven_err_t rp_msix_appinstaller(proven_allocator_t alloc, const rp_ir_t *ir, const rp_msix_options_t *opt, bool bundle,
                                                uint8_t **out, size_t *len);

// A package going into a bundle: its file name there (`<name>_<version>_<arch>.msix`, say) and bytes.
typedef struct {
    const char    *file_name;
    const uint8_t *data;
    size_t         len;
} rp_msix_part_t;

// A .msixbundle of packages (RFC-0010 P8b-2), as Windows' bundle writer lays it out: every package
// must open (rp_msix_open), all with one Name, Publisher and Version and each of its own
// architecture; the bundle's version is theirs. PROVEN_ERR_INVALID_ARG with *why otherwise.
[[nodiscard]] proven_err_t rp_msix_bundle(proven_allocator_t alloc, const rp_msix_part_t *parts, size_t n, const rp_limits_t *lim,
                                          uint8_t **out, size_t *len, const char **why);

// A payload file of a package, as its block map names it; for a bundle, its manifest and packages.
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
// A bundle (AppxMetadata/AppxBundleManifest.xml, no AppxManifest.xml) opens too: *manifest gets the
// bundle manifest, *files its manifest and then its packages, each checked where the manifest says
// it lies, opened as a package, and of the identity the manifest gives it; one per architecture.
[[nodiscard]] proven_err_t rp_msix_open(proven_allocator_t alloc, const uint8_t *pkg, size_t len, const rp_limits_t *lim,
                                        rp_msix_file_t **files, size_t *count, uint8_t **manifest, size_t *manifest_len,
                                        const char **why);
void rp_msix_files_free(proven_allocator_t alloc, rp_msix_file_t *files, size_t count);

// The value of `attribute` on the first `element` (matched by its full name, prefix included) in
// an XML text, decoded; false when there is none or it does not fit.
[[nodiscard]] bool rp_xml_attr(const char *xml, size_t len, const char *element, const char *attribute, char *out, size_t cap);

// Signs a package or bundle (RFC-0011 P9a; src/msix/msix_sign.c) as Windows' AppX SIP does: adds
// AppxSignature.p7x after [Content_Types].xml (which gains its Override); a bundle's packages are
// signed first and a new bundle made around them. The manifest's Publisher must be the signing
// certificate's subject as Windows writes it (last RDN first). A signed package is refused.
[[nodiscard]] proven_err_t rp_msix_sign(proven_allocator_t alloc, const uint8_t *pkg, size_t len, const rp_keyfile_t *kf, int64_t now,
                                        const rp_timestamper_t *ts, uint8_t **out, size_t *out_len, const char **why);
// The MSIX Publisher a certificate (DER) signs for: its subject written the way Windows writes it
// (the RDNs last to first, "A=v, B=v"). False when it cannot be written.
[[nodiscard]] bool rp_msix_publisher_of(proven_allocator_t alloc, const uint8_t *cert, size_t len, char *out, size_t cap);

// Checks a signed package or bundle: the package itself (rp_msix_open), the signature, and the
// AppX record of hashes against the file (AXCI when it has a CodeIntegrity.cat); for a bundle each
// package's own signature too. The spans in *r point into *sig (free with rp_mem_free).
void rp_msix_verify(proven_allocator_t alloc, const uint8_t *pkg, size_t len, rp_authenticode_check_t *r, uint8_t **sig,
                    const char **why);

// `rubrapack inspect|lint <file.msix>` (src/cli/msix.c).
[[nodiscard]] int rp_msix_inspect(const char *path, const char *what);
[[nodiscard]] int rp_msix_lint(const char *path, bool strict);

// A merge module's part an MSIX can carry (src/msix/msix_merge.c, RFC-0019): its files, each below
// the module's root (vfs NULL) or below a standard folder (vfs: its VFS folder), and its registry
// values as [registry.*] tables (id NULL: the caller names them). Configured with `config`.
typedef struct {
    char       *rel;            // "sub\\file.txt"
    const char *vfs;            // "SystemX64" ..., or NULL
    uint8_t    *data;
    size_t      len;
} rp_msix_module_file_t;

typedef struct {
    rp_msix_module_file_t *files;
    size_t                 file_count;
    rp_ir_registry_t      *regs;
    size_t                 reg_count;
} rp_msix_module_t;

[[nodiscard]] proven_err_t rp_msix_module_read(proven_allocator_t alloc, const char *path, const char *const *config, size_t nconfig,
                                               rp_msix_module_t *out, const char **why);
void rp_msix_module_free(proven_allocator_t alloc, rp_msix_module_t *x);

#endif // RUBRAPACK_MSIX_H
