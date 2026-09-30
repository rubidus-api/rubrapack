#ifndef RUBRAPACK_CHAIN_H
#define RUBRAPACK_CHAIN_H

// include/rubrapack/chain.h - several packages installed in order by one setup program (RFC-0016 3).
// A source with [chain] and [chain-package.ID] tables builds `setup.exe`: rubrapack's bootstrapper
// (src/setup/rubrapack_setup.c) followed by the payload -
//   the packages, back to back;
//   the manifest: "RPCHAIN1", u32 package count, u32 flags (1 = elevate), the chain's name,
//     manufacturer and version, then per package its ID, msiexec properties, ProductCode and
//     ProductVersion (each u32 length + UTF-8), u32 flags (1 = vital), u64 offset (from the first
//     package), u64 size, SHA-256;
//   zero bytes up to a multiple of 8 for the whole file, so a signature can follow directly;
//   a 32-byte trailer: u64 file offset of the first package, u64 file offset of the manifest, u32
//     manifest size, u32 0, "RPCHAIN!".
// All numbers little-endian.

#include <stddef.h>
#include <stdint.h>

#include "proven/allocator.h"
#include "proven/types.h"
#include "rubrapack/ir.h"
#include "rubrapack/limits.h"
#include "rubrapack/srcdiag.h"
#include "rubrapack/toml.h"

enum { RP_CHAIN_MAX = 64 };

typedef struct {
    char    *id;            // [chain-package.ID]: also the package's file name (ID.msi) while installing
    char    *source;        // the .msi, joined with the source's folder
    char    *shown;         // as written
    char    *properties;    // msiexec properties ("NAME=value ..."), or NULL
    bool     vital;         // a failure stops the chain (default)
    rp_pos_t pos;
} rp_chain_pkg_t;

typedef struct {
    proven_allocator_t alloc;
    char              *name, *manufacturer, *version;
    rp_arch_t          arch;        // the bootstrapper's architecture
    bool               elevate;     // run elevated (default): per-machine packages need it
    rp_chain_pkg_t    *pkgs;
    size_t             count;
} rp_chain_t;

// Whether the document is a chain (has a [chain] table).
[[nodiscard]] bool rp_chain_is(const rp_tdoc_t *doc);

// Checks the document ([chain], [chain-package.ID], [define] only) and builds the model. On any
// error the diagnostics say why and PROVEN_ERR_INVALID_FORMAT is returned.
[[nodiscard]] proven_err_t rp_chain_parse(proven_allocator_t alloc, const rp_tdoc_t *doc, const rp_ir_options_t *opt,
                                          rp_chain_t *out, rp_srcdiags_t *diags);
void rp_chain_free(rp_chain_t *c);

// Writes the setup program: reads every package (each must be an MSI with a ProductCode) and
// appends it with the manifest to the bootstrapper for c->arch. Problems go to the diagnostics.
[[nodiscard]] proven_err_t rp_chain_write(proven_allocator_t alloc, const rp_chain_t *c, const rp_limits_t *limits,
                                          uint8_t **out, size_t *len, rp_srcdiags_t *diags);

typedef struct {
    const char    *id, *properties, *product_code, *version;     // NUL-terminated copies
    bool           vital;
    const uint8_t *data;        // the package's bytes, inside the setup program
    uint64_t       size;
    bool           hash_ok;     // its SHA-256 matches the manifest
} rp_chain_entry_t;

// Reads a setup program's payload (a signed one too): the chain's name and its packages. *entries
// is one allocation (strings included); free it with rp_mem_free. PROVEN_ERR_NOT_FOUND when the
// file is not a rubrapack setup program.
[[nodiscard]] proven_err_t rp_chain_read(proven_allocator_t alloc, const uint8_t *exe, size_t len, char *name, size_t name_cap,
                                         rp_chain_entry_t **entries, size_t *count);

#endif // RUBRAPACK_CHAIN_H
