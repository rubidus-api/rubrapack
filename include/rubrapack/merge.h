// include/rubrapack/merge.h - merge modules (.msm) merged into a package (RFC-0016 3).

#ifndef RUBRAPACK_MERGE_H
#define RUBRAPACK_MERGE_H

#include "rubrapack/cab.h"
#include "rubrapack/cfb.h"
#include "rubrapack/ir.h"
#include "rubrapack/msi.h"

// An opened merge module. Its tables stay readable (the cells merged into a package point into it)
// until rp_msi_module_close.
typedef struct {
    proven_allocator_t alloc;
    uint8_t           *data;
    size_t             len;
    rp_cfb_t           cfb;
    rp_msi_t           msi;
    rp_msi_view_t      view;
    bool               cfb_open, msi_open, view_open;
    char               module_id[128];  // ModuleSignature.ModuleID: <name>.<GUID with underscores>
    uint8_t           *cab;             // MergeModule.CABinet, or NULL
    size_t             cab_len;
    void             **kept;            // allocations the merged tables use
    size_t             nkept, capkept;
} rp_msi_module_t;

// Reads a merge module. *why says what is wrong (the caller names the file).
[[nodiscard]] proven_err_t rp_msi_module_open(proven_allocator_t alloc, const char *path, rp_msi_module_t *m, const char **why);
void rp_msi_module_close(rp_msi_module_t *m);

// The module's _Validation table (for the tables it adds), or NULL.
const rp_msi_wtable_t *rp_msi_module_validation(const rp_msi_module_t *m);

// Merges the module into tables[0..*nt) (room for `cap`): rows appended, new tables added, its
// TARGETDIR's children under dir_key, its components in `feature`, its actions placed. When it
// has files, *cab_stream is the cabinet to embed as msm<index>.cab (a new Media row names it).
[[nodiscard]] proven_err_t rp_msi_module_merge(rp_msi_module_t *m, rp_msi_wtable_t *tables, size_t *nt, size_t cap,
                                               const char *dir_key, const char *feature, unsigned index,
                                               rp_msi_wstream_t *cab_stream, const char **why);

// Turns the tables of a lowered [module] source into a merge module and writes it (RFC-0017;
// src/msi/module_out.c): the module's keys get ".<GUID>", ModuleSignature, ModuleComponents and
// ModuleInstallExecuteSequence are added, the package-only tables left out, and `files` (named by
// their keys without the GUID) go into MergeModule.CABinet. *why says what is wrong.
[[nodiscard]] proven_err_t rp_msm_write(proven_allocator_t alloc, const rp_ir_t *ir, const rp_msi_wtable_t *tables, size_t count,
                                        const rp_cab_file_t *files, size_t nfiles, size_t jobs, const rp_limits_t *limits,
                                        const rp_out_sink_t *sink, uint8_t **out, size_t *len, const char **why);

#endif // RUBRAPACK_MERGE_H
