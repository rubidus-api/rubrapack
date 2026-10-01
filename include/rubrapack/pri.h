#ifndef RUBRAPACK_PRI_H
#define RUBRAPACK_PRI_H

// include/rubrapack/pri.h - the package resource index of an MSIX (resources.pri; RFC-0016 3): which
// string or file a resource name gives for a language or a display scale. Written in the layout
// Windows SDK's makepri.exe writes (format notes F8 "resources.pri"), for what a package manifest
// needs: strings under Resources/ (the manifest says ms-resource:NAME) and files under Files/ with
// scale variants (Assets\Logo.png finds Assets\Logo.scale-200.png).

#include <stddef.h>
#include <stdint.h>

#include "proven/allocator.h"
#include "proven/types.h"

enum { RP_PRI_NONE, RP_PRI_LANGUAGE, RP_PRI_SCALE };

typedef struct {
    const char *name;       // "Resources/AppDisplayName" or "Files/Assets/Logo.png": ASCII, '/' between parts
    int         qualifier;  // RP_PRI_*
    const char *qvalue;     // "en-US", "ko"; "100", "125", "150", "200", "400"; NULL for none
    bool        path;       // the value is a file in the package, else a string
    const char *value;      // UTF-8 string, or the file's package path ("Assets\\Logo.scale-200.png")
} rp_pri_candidate_t;

// Writes the index for the package `identity` (its Identity Name). A resource with qualified
// candidates must not also have an unqualified one; a scale must be one of the five above.
// Deterministic.
[[nodiscard]] proven_err_t rp_pri_write(proven_allocator_t alloc, const char *identity, const char *default_language,
                                        const rp_pri_candidate_t *cands, size_t count, uint8_t **out, size_t *len);

// The same index for one part of the resources (RFC-0019, as makepri splits languages into resource
// packages): part "" keeps every candidate but those of languages other than the default (the
// main package's), part "ko" only that language's (its resource package). Every resource name
// stays in the index; one without candidates in the part has none (decision 0). NULL: everything.
[[nodiscard]] proven_err_t rp_pri_write_part(proven_allocator_t alloc, const char *identity, const char *default_language,
                                             const rp_pri_candidate_t *cands, size_t count, const char *part, uint8_t **out, size_t *len);

#endif // RUBRAPACK_PRI_H
