#ifndef RUBRAPACK_PE_H
#define RUBRAPACK_PE_H

// include/rubrapack/pe.h - Portable Executable facts a package needs (RFC-0001 F7): the machine
// type (checked against the package architecture) and the version resource (File.Version and
// File.Language instead of a file hash).

#include <stddef.h>
#include <stdint.h>

#include "proven/types.h"

enum {
    RP_PE_I386 = 0x014C,
    RP_PE_AMD64 = 0x8664,
    RP_PE_ARM64 = 0xAA64,
};

typedef struct {
    bool     is_pe;             // "MZ" + a valid "PE\0\0" header
    uint16_t machine;           // COFF machine
    bool     is_dll;            // COFF characteristics IMAGE_FILE_DLL (0x2000)
    bool     has_version;       // VS_FIXEDFILEINFO found
    uint16_t version[4];        // file version a.b.c.d
    uint16_t language;          // first Translation language, else the resource language, else 0
} rp_pe_info_t;

// Reads what it can. A file that is not PE gives is_pe = false and PROVEN_OK; a PE file whose
// headers or resources point outside the file gives PROVEN_ERR_INVALID_FORMAT.
[[nodiscard]] proven_err_t rp_pe_read(const uint8_t *data, size_t len, rp_pe_info_t *info);

#endif // RUBRAPACK_PE_H
