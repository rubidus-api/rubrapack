#ifndef RUBRAPACK_IR_H
#define RUBRAPACK_IR_H

// include/rubrapack/ir.h - the checked package model built from a `.rpk` AST (RFC-0002 section 9).
// Everything here has passed the table/key/type/path checks; back ends (MSI lowering) never look
// at the source text again. Strings are UTF-8 and owned by the IR.

#include <stddef.h>
#include <stdint.h>

#include "proven/allocator.h"
#include "proven/types.h"
#include "rubrapack/srcdiag.h"
#include "rubrapack/toml.h"

typedef enum { RP_ARCH_X64, RP_ARCH_ARM64, RP_ARCH_X86 } rp_arch_t;

typedef struct {
    char    *id;
    char    *title;
    char    *description;       // may be NULL
    int32_t  level;
    bool     hidden;
    char    *parent;            // feature ID or NULL
    bool     implicit;          // the default `Main` feature (G2)
    rp_pos_t pos;
} rp_ir_feature_t;

typedef struct {
    char    *id;
    char    *base;              // known folder name (ProgramFiles, ...) or NULL when `parent` is set
    char    *parent;            // dir ID or NULL
    char   **parts;             // relative path components below base/parent (at least one)
    size_t   part_count;
    char    *feature;           // may be NULL
    bool     implicit;          // a sub folder created for [files.*] matches below a wildcard
    rp_pos_t pos;
} rp_ir_dir_t;

typedef struct {
    char    *id;                // logical ID
    char    *dir;               // dir ID
    char    *source;            // path as written (relative to the .rpk directory, `/`)
    char    *source_path;       // path to open (joined with the .rpk directory)
    char    *name;              // target file name
    uint64_t size;
    bool     any_arch, keep, vital;
    uint16_t pe_machine;        // 0 when the file is not a PE program (checked in rp_ir_build)
    bool     pe_is_dll;
    char    *feature;           // resolved (G2): never NULL
    char    *component_guid;    // user-fixed GUID or NULL
    rp_pos_t pos;
} rp_ir_file_t;

typedef struct {
    char    *id;
    char    *dir;               // dir ID
    char    *name;              // folder name
    bool     keep;              // left behind at uninstall
    char    *feature;           // resolved (G2)
    rp_pos_t pos;
} rp_ir_folder_t;

typedef struct {
    char    *id;                // public property name (upper case)
    char    *value;
    bool     secure, hidden;    // SecureCustomProperties / MsiHiddenProperties
    rp_pos_t pos;
} rp_ir_property_t;

// A do/undo pair run from an installed exe (RFC-0003): `do` at install and repair, `undo` at
// removal; the lowering adds conditions, order and rollback twins.
typedef struct {
    char    *id;
    char    *run_file;          // file ID of the exe (from run = "file:ID")
    char    *do_args, *undo_args;
    char    *check_args;        // may be NULL
    rp_pos_t pos;
} rp_ir_action_t;

typedef struct {
    // [package]
    char     *name, *summary_name, *manufacturer;
    char     *version;          // as written, validated
    uint16_t  version_parts[4];
    size_t    version_count;
    rp_arch_t arch;
    char     *upgrade_code;     // upper case, braces
    char     *product_code;     // NULL = derive
    uint16_t  language;         // 1033 or 1042
    bool      reboot_suppress;
    char     *downgrade_message;
    char     *refuse_below;     // refuse-upgrade-below (versions below it are refused), or NULL
    char     *refuse_message;   // may be NULL
    int       compress;         // -1 none, 0-9 MSZIP level

    rp_ir_feature_t *features;
    size_t           feature_count;
    rp_ir_dir_t     *dirs;
    size_t           dir_count;
    rp_ir_file_t    *files;
    size_t           file_count;
    rp_ir_folder_t  *folders;
    size_t           folder_count;
    rp_ir_property_t *properties;
    size_t            property_count;
    rp_ir_action_t   *actions;
    size_t            action_count;
    // [arp]
    bool      arp_no_modify, arp_no_repair;
    char     *arp_help, *arp_about;     // may be NULL
    proven_allocator_t alloc;
} rp_ir_t;

typedef struct {
    const char *name;
    const char *value;
} rp_define_t;

typedef struct {
    const char        *source_dir;      // directory of the .rpk file ("." when none)
    const rp_define_t *defines;         // -D NAME=VALUE, already checked for duplicates
    size_t             define_count;
    const char        *arch;            // --arch, or NULL
    const char        *compress;        // --compress, or NULL
    const char        *output;          // the file being built (refused inside a glob), or NULL
} rp_ir_options_t;

// Checks the document and builds the model. On any error the diagnostics say why, the model is
// empty, and PROVEN_ERR_INVALID_FORMAT is returned.
[[nodiscard]] proven_err_t rp_ir_build(proven_allocator_t alloc, const rp_tdoc_t *doc, const rp_ir_options_t *opt,
                                       rp_ir_t *ir, rp_srcdiags_t *diags);
void rp_ir_free(rp_ir_t *ir);

// A stable text dump of the model for golden tests.
[[nodiscard]] proven_err_t rp_ir_dump(const rp_ir_t *ir, proven_allocator_t alloc, uint8_t **out, size_t *len);

#endif // RUBRAPACK_IR_H
