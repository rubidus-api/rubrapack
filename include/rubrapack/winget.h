#ifndef RUBRAPACK_WINGET_H
#define RUBRAPACK_WINGET_H

// include/rubrapack/winget.h - `build --winget DIR` (RFC-0031 B28): the Windows Package Manager's
// manifest of a package rubrapack just wrote - version, installer and default-locale files (and a
// locale file for each short-description-xx), schema 1.12.0. What the package itself says
// (ProductCode, UpgradeCode, the architecture, the package family name, the hashes) is read back
// from the file as written; the rest comes from the source's [winget] table.

#include <stddef.h>

#include "proven/allocator.h"
#include "rubrapack/ir.h"

typedef struct {
    char  *lang, *text;
} rp_winget_text_t;

typedef struct {
    char            *id, *url, *license, *desc, *publisher_url, *package_url, *license_url, *moniker;
    char           **tags;
    size_t           tag_count;
    rp_winget_text_t *descs;            // short-description-xx
    size_t           desc_count;
    char            *name, *publisher;  // PackageName and Publisher: what Installed apps shows
    const char      *locale;            // the package's language: "en-US" or "ko-KR"
    int              scope;             // [package] scope: 0 machine, 1 user, 2 dual (no Scope)
} rp_winget_t;

// Copies what the manifest needs from a model (its [winget] table, name, publisher, language);
// false when out of memory. Free with rp_winget_free.
[[nodiscard]] bool rp_winget_take(proven_allocator_t alloc, const rp_ir_t *ir, rp_winget_t *w);
void rp_winget_free(proven_allocator_t alloc, rp_winget_t *w);

// Writes the manifest of the package at `pkg` (a .msi, .msix or .msixbundle) into `dir` (made when
// it is not there): <id>.yaml, <id>.installer.yaml, <id>.locale.<locale>.yaml. Prints why not and
// returns an exit code.
[[nodiscard]] int rp_winget_write(proven_allocator_t alloc, const rp_winget_t *w, const char *pkg, const char *dir);

#endif // RUBRAPACK_WINGET_H
