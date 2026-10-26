#ifndef RUBRAPACK_BUILD_H
#define RUBRAPACK_BUILD_H

// include/rubrapack/build.h - IR -> Windows Installer package (RFC-0001 section 9, P2 scope:
// machine scope, files, folders, features, upgrade/downgrade, stored cabinet).

#include <stddef.h>
#include <stdint.h>

#include "proven/allocator.h"
#include "proven/types.h"
#include "rubrapack/cfb.h"
#include "rubrapack/ir.h"
#include "rubrapack/limits.h"
#include "rubrapack/srcdiag.h"

// RFC-0031 B13: signs the external cabinets of a package built with --key. `sign` signs a cabinet
// that has the room (rp_cab_prepare; it reports its own errors and sets `failed`); `cert` is the
// signing certificate (DER) for MsiDigitalCertificate. Every external cabinet gets a
// MsiDigitalSignature row whose Hash is its signature's value (what MsiGetFileSignatureInformation
// gives, tests/fixtures/x98), so Windows Installer checks it before using it. The values are kept
// in `hash` by DiskId - 1 (freed with rp_cab_signer_free); a signer without `sign` takes them from
// there (an instance's variant, only compared with the package).
enum { RP_CAB_SIGNED_MAX = 256 };
typedef struct {
    proven_err_t (*sign)(void *ctx, const uint8_t *cab, size_t len, uint8_t **out, size_t *out_len);
    void          *ctx;
    const uint8_t *cert;
    size_t         cert_len;
    bool           failed;
    uint8_t       *hash[RP_CAB_SIGNED_MAX];
    size_t         hash_len[RP_CAB_SIGNED_MAX];
} rp_cab_signer_t;
void rp_cab_signer_free(proven_allocator_t alloc, rp_cab_signer_t *s);

typedef struct {
    bool        reproducible;   // package code from the content instead of random (RFC-0001 14.2)
    const char *cab_stem;       // external cabinets are named <stem>.cab / <stem>-N.cab (cab = "external")
    size_t      jobs;           // threads compressing cabinets; 0 = the processors (RFC-0013 E2)
    const rp_out_sink_t *sink;  // where the MSI bytes go (rubrapack/cfb.h); NULL = the heap (RFC-0013 R2b)
    rp_cab_signer_t *cab_signer;    // NULL: external cabinets unsigned
} rp_build_options_t;

// A file written next to the package (an external cabinet). Free with rp_build_files_free.
typedef struct {
    char    *name;
    uint8_t *data;
    size_t   len;
} rp_build_file_t;

void rp_build_files_free(proven_allocator_t alloc, rp_build_file_t *files, size_t count);

// Reads every source file, then writes the MSI bytes to *out (free with rp_mem_free, or through
// opt->sink's drop when a sink gave the buffer) and, with
// cab = "external", the cabinets to *cabs (to be written next to the package before it).
// Problems found while lowering (unreadable file, too many files, ...) go to `diags`, and so do
// lint findings (rubrapack/lint.h), which return PROVEN_ERR_INVALID_STATE and write nothing.
[[nodiscard]] proven_err_t rp_msi_from_ir(proven_allocator_t alloc, const rp_ir_t *ir, const rp_build_options_t *opt,
                                          const rp_limits_t *limits, uint8_t **out, size_t *len,
                                          rp_build_file_t **cabs, size_t *cab_count, rp_srcdiags_t *diags);

#endif // RUBRAPACK_BUILD_H
